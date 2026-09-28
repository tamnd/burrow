/* Derived from Go's src/compress/flate/deflate_test.go, writer_test.go,
 * huffman_bit_writer_test.go and deflate_fast_test.go, the writer side of them.
 * Go source: go1.27.1.
 *
 * Go's huffman bit writer tests read their input and expected output from
 * files in testdata. Here the inputs, the tokens and the SHA-256 of the
 * expected output are in tests/flate_writer_test_gen.h, from
 * tools/gen-flate-writer-tests.sh, which also records what Go's writer makes
 * of a set of inputs at every level and with several ways of writing them.
 * TestMaxStackSize is about goroutine stacks and has no counterpart. The Isaac
 * Newton text the persistent error tests use is words here, and e.txt and
 * Opticks in TestDeflateInflateString are covered by the generated vectors.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/compress/flate_internal.h"

#include "burrow/burrow.h"
#include "burrow/compress/flate.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/base64.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"

#include "flate_test_gen.h"
#include "flate_writer_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Slice no_dict = {NULL, 0, 0, NULL};

/* A generated stream, joined and decoded. */
static Slice gen_bytes(Alloc *a, const char *const *lines) {
    Int n = 0;
    for (const char *const *l = lines; *l != NULL; l++)
        n += (Int)strlen(*l);
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    Int off = 0;
    for (const char *const *l = lines; *l != NULL; l++) {
        size_t k = strlen(*l);
        memcpy(p + off, *l, k);
        off += (Int)k;
    }
    Error err;
    Slice out = base64_encoding_decode_string(base64_std_encoding, a,
                                              str_from_bytes(p, n), &err);
    if (BURROW_FAILED(err)) {
        fprintf(stderr, "bad generated stream\n");
        abort();
    }
    return out;
}

/* The words tools/gen-flate-tests.sh compresses, made the same way. */
static Slice flate_words(Alloc *a, Int n) {
    static const char *const vocab[] = {
        "the",  "quick", "brown", "fox",  "jumped", "over",  "lazy",  "dog",
        "and",  "a",     "of",    "to",   "in",     "is",    "that",  "it",
        "was",  "for",   "on",    "are",  "as",     "with",  "his",   "they",
        "at",   "be",    "this",  "from", "I",      "have",  "or",    "by",
        "one",  "had",   "not",   "but",  "what",   "all",   "were",  "when",
        "we",   "there", "can",   "an",   "your",   "which", "their", "said",
        "if",   "do",    "will",  "each", "about",  "how",   "up",    "out",
        "them", "then",  "she",   "many", "some",   "so",    "these", "would",
    };
    const uint64_t nv = sizeof vocab / sizeof vocab[0];
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 16, 1);
    Int len = 0;
    uint64_t x = 1;
    while (len < n) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        const char *w = vocab[(x >> 33) % nv];
        size_t k = strlen(w);
        for (size_t i = 0; i < k && len < n; i++)
            p[len++] = (Byte)w[i];
        if (len < n)
            p[len++] = x >> 60 == 0 ? '\n' : ' ';
    }
    return slice_from(p, n, n, TYPE_BYTE);
}

/* Compresses in at level, after dict when it is not nil, the way the Go
 * generators do: one Write, an optional Flush, then Close. */
static Slice deflate_all(Alloc *a, int level, Slice dict, bool flush, Slice in) {
    BytesBuffer out = BYTES_BUFFER(a);
    Error err;
    FlateWriter *w =
        dict.p != NULL
            ? flate_new_writer_dict(a, bytes_buffer_as_io_writer(&out), level, dict,
                                    &err)
            : flate_new_writer(a, bytes_buffer_as_io_writer(&out), level, &err);
    if (w == NULL) {
        fprintf(stderr, "flate_new_writer: %.*s\n", (int)error_text(err).len,
                (const char *)error_text(err).p);
        abort();
    }
    Int n = flate_writer_write(w, in, &err);
    if (n != in.len || BURROW_FAILED(err))
        abort();
    if (flush && BURROW_FAILED(flate_writer_flush(w)))
        abort();
    if (BURROW_FAILED(flate_writer_close(w)))
        abort();
    flate_writer_free(w);
    return bytes_buffer_bytes(&out);
}

static bool bytes_eq(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

#define BS(lit)                                                                        \
    slice_from((void *)(uintptr_t)(lit), (Int)sizeof(lit) - 1, (Int)sizeof(lit) - 1,   \
               TYPE_BYTE)

static Slice bytes_of(Alloc *a, const Byte *p, Int n) {
    Byte *q = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    for (Int i = 0; i < n; i++)
        q[i] = p[i];
    return slice_from(q, n, n, TYPE_BYTE);
}

static Slice make_bytes(Alloc *a, Int n) {
    return slice_from(mem_alloc(a, (size_t)n + 1, 1), n, n, TYPE_BYTE);
}

static Byte *bp(Slice s) {
    return (Byte *)s.p;
}

static bool text_is(Str s, const char *want) {
    return (size_t)s.len == strlen(want) && memcmp(s.p, want, (size_t)s.len) == 0;
}

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static void sha_hex(Slice b, char out[65]) {
    static const char digits[] = "0123456789abcdef";
    Sha256Sum256Ret h = sha256_sum256(b);
    for (int i = 0; i < 32; i++) {
        out[2 * i] = digits[h.a[i] >> 4];
        out[2 * i + 1] = digits[h.a[i] & 15];
    }
    out[64] = 0;
}

/* io.ReadAll of a reader over the decompressed form of in, after dict. */
static Slice inflate_dict(Alloc *a, Slice in, Slice dict, Error *err) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    IoReadCloser rc = flate_new_reader_dict(a, bytes_reader_as_io_reader(&br), dict);
    Slice out = io_read_all(a, io_read_closer_as_io_reader(rc), err);
    flate_reader_free(rc);
    return out;
}

static Slice inflate_all(Alloc *a, Slice in, Error *err) {
    return inflate_dict(a, in, no_dict, err);
}

/* Bytes from the generator the words use, seeded, the top byte of each step,
 * as noise in tools/gen-flate-writer-tests.sh. */
static Slice noise(Alloc *a, Int n, uint64_t seed, Byte mask) {
    Slice b = make_bytes(a, n);
    uint64_t x = seed;
    for (Int i = 0; i < n; i++) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        bp(b)[i] = (Byte)(x >> 56) & mask;
    }
    return b;
}

/* The inputs the generator numbers. */
static Slice flate_input(Alloc *a, int id) {
    switch (id) {
    case 0:
        return gen_bytes(a, gettysburg);
    case 1:
        return flate_words(a, 34000);
    case 2:
        return flate_words(a, 300000);
    case 3:
        return noise(a, 70000, 7, 0xff);
    case 4:
        return noise(a, 65535 * 3 + 500, 1, 7);
    case 5: {
        Slice b = make_bytes(a, 100000);
        for (Int i = 0; i < b.len; i++)
            bp(b)[i] = (Byte)(i * i & 0xff);
        return b;
    }
    case 6: {
        Slice b = make_bytes(a, 100000);
        memset(b.p, 0, 100000);
        return b;
    }
    case 7: {
        Slice b = make_bytes(a, 131072);
        for (Int i = 0; i < b.len; i++)
            bp(b)[i] = (Byte)(i % 128);
        return b;
    }
    case 8: {
        Slice wd = flate_words(a, 100000);
        Slice nz = noise(a, 100000, 3, 0xff);
        Slice b = make_bytes(a, 200000);
        Int n = 0;
        for (Int i = 0; i < 100000; i += 5000) {
            memcpy(bp(b) + n, bp(wd) + i, 5000);
            memcpy(bp(b) + n + 5000, bp(nz) + i, 5000);
            n += 10000;
        }
        return b;
    }
    case 9:
        return no_dict;
    default:
        return bytes_of(a, (const Byte *)"hello, hello, hello, hello\n", 27);
    }
}

static const char small_dict_text[] =
    "we are the world - how are you?we are the world - how are you?"
    "we are the world - how are you?";

/* Writes in at level the way pattern says, as run in the generator does. */
static Slice flate_pattern(Alloc *a, int level, int pattern, Slice in) {
    BytesBuffer out = BYTES_BUFFER(a);
    IoWriter ow = bytes_buffer_as_io_writer(&out);
    Error err;
    FlateWriter *w;
    if (pattern == 4)
        w = flate_new_writer_dict(a, ow, level, flate_words(a, 34000), &err);
    else if (pattern == 5)
        w = flate_new_writer_dict(a, ow, level, BS(small_dict_text), &err);
    else
        w = flate_new_writer(a, ow, level, &err);
    if (w == NULL)
        abort();
    Int chunk = in.len;
    bool flush = false;
    if (pattern == 1)
        chunk = 787;
    else if (pattern == 2)
        chunk = 81761;
    else if (pattern == 3)
        chunk = 10000, flush = true;
    else if (pattern == 6)
        chunk = 1;
    if (chunk == 0)
        chunk = 1;
    for (Int i = 0; i < in.len; i += chunk) {
        Int hi = i + chunk < in.len ? i + chunk : in.len;
        flate_writer_write(w, slice_sub(in, i, hi), &err);
        if (flush)
            (void)flate_writer_flush(w);
    }
    if (in.len == 0)
        flate_writer_write(w, in, &err);
    (void)flate_writer_close(w);
    flate_writer_free(w);
    return bytes_buffer_bytes(&out);
}

/* A writer that fails with io_err_closed_pipe once n writes have gone
 * through, Go's errorWriter. */
typedef struct ErrorWriter {
    int n;
} ErrorWriter;

static Int error_writer_write(void *self, Slice p, Error *err) {
    ErrorWriter *e = (ErrorWriter *)self;
    if (e->n <= 0) {
        *err = io_err_closed_pipe;
        return 0;
    }
    e->n--;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT error_writer_vt = {NULL, error_writer_write};

/* Go's failWriter: fails with err_io exactly at the nth call to Write. */
typedef struct FailWriter {
    int n;
} FailWriter;

static const Str err_io_text = {(const Byte *)"IO error", 8};
static const Error err_io = {&burrow_sentinel_error_vt, &err_io_text};

static Int fail_writer_write(void *self, Slice p, Error *err) {
    FailWriter *f = (FailWriter *)self;
    f->n--;
    if (f->n == -1) {
        *err = err_io;
        return 0;
    }
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT fail_writer_vt = {NULL, fail_writer_write};

/* A reader with only Read, Go's struct{ io.Reader }. */
typedef struct PlainReader {
    IoReader r;
} PlainReader;

static Int plain_read(void *self, Slice p, Error *err) {
    PlainReader *pr = (PlainReader *)self;
    return pr->r.vt->read(pr->r.data, p, err);
}

static const IoReaderVT plain_reader_vt = {NULL, plain_read};

/* Go's writer_test.go input, n lines of "asdasfasf%d%dfghfgujyut%dyutyu\n"
 * or, with fast set, "asdfasdfasdfasdf%d%dfghfgujyut%dyutyu\n". */
static Slice asd_lines(Alloc *a, int n, bool fast) {
    BytesBuffer b = BYTES_BUFFER(a);
    char line[96];
    for (int i = 0; i < n; i++) {
        int k = snprintf(line, sizeof line,
                         fast ? "asdfasdfasdfasdf%d%dfghfgujyut%dyutyu\n"
                              : "asdasfasf%d%dfghfgujyut%dyutyu\n",
                         i, i, i);
        bytes_buffer_write(&b, slice_from(line, k, k, TYPE_BYTE), NULL);
    }
    return bytes_buffer_bytes(&b);
}

/* ------------------------------------------------------- deflate_test.go */

typedef struct DeflateTest {
    const char *in;
    Int in_len;
    int level;
    const char *out;
    Int out_len;
} DeflateTest;

#define DT(in, level, out) {in, (Int)sizeof(in) - 1, level, out, (Int)sizeof(out) - 1}

static const DeflateTest deflate_tests[] = {
    DT("", 0, "\x03\x00"),
    DT("\x11", FLATE_BEST_COMPRESSION, "\x12\x04\x0c\x00"),
    DT("\x11", FLATE_BEST_COMPRESSION, "\x12\x04\x0c\x00"),
    DT("\x11", FLATE_BEST_COMPRESSION, "\x12\x04\x0c\x00"),
    DT("\x11", 0, "\x00\x01\x00\xfe\xff\x11\x03\x00"),
    DT("\x11\x12", 0, "\x00\x02\x00\xfd\xff\x11\x12\x03\x00"),
    DT("\x11\x11\x11\x11\x11\x11\x11\x11", 0,
       "\x00\x08\x00\xf7\xff\x11\x11\x11\x11\x11\x11\x11\x11\x03\x00"),
    DT("", 1, "\x03\x00"),
    DT("\x11", FLATE_BEST_COMPRESSION, "\x12\x04\x0c\x00"),
    DT("\x11\x12", FLATE_BEST_COMPRESSION, "\x12\x14\x02\x0c\x00"),
    DT("\x11\x11\x11\x11\x11\x11\x11\x11\x11", FLATE_BEST_COMPRESSION,
       "\x12\x84\x01\xc0\x00"),
    DT("", 9, "\x03\x00"),
    DT("\x11", 9, "\x12\x04\x0c\x00"),
    DT("\x11\x12", 9, "\x12\x14\x02\x0c\x00"),
    DT("\x11\x11\x11\x11\x11\x11\x11\x11\x11", 9, "\x12\x84\x01\xc0\x00"),
};

#define NDEFLATE_TESTS (sizeof deflate_tests / sizeof deflate_tests[0])

static Slice large_data_chunk(Alloc *a) {
    Slice b = make_bytes(a, 100000);
    for (Int i = 0; i < b.len; i++)
        bp(b)[i] = (Byte)(i * i & 0xff);
    return b;
}

static void TestBulkHash4(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t k = 0; k < NDEFLATE_TESTS; k++) {
        const DeflateTest *x = &deflate_tests[k];
        if (x->out_len < 4)
            continue;
        Int n = 2 * x->out_len;
        Byte *y = (Byte *)mem_alloc(a, (size_t)n, 1);
        memcpy(y, x->out, (size_t)x->out_len);
        memcpy(y + x->out_len, x->out, (size_t)x->out_len);
        for (Int j = 4; j < n; j++) {
            Int nd = j - 4 + 1;
            uint32_t *dst = (uint32_t *)mem_alloc(a, (size_t)nd * 4, 4);
            for (Int i = 0; i < nd; i++)
                dst[i] = (uint32_t)(i + 100);
            burrow__flate_bulk_hash4(y, j, dst);
            for (Int i = 0; i < nd; i++) {
                uint32_t want = burrow__flate_hash4(y + i);
                if (dst[i] != want && dst[i] == (uint32_t)i + 100)
                    testing_t_errorf_v(t,
                                       "Len:%d Index:%d, want 0x%08x but not modified",
                                       (int)j, (int)i, want);
                else if (dst[i] != want)
                    testing_t_errorf_v(t, "Len:%d Index:%d, got 0x%08x want:0x%08x",
                                       (int)j, (int)i, dst[i], want);
            }
        }
    }
    arena_free(&ar);
}

static void TestDeflate(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NDEFLATE_TESTS; i++) {
        const DeflateTest *h = &deflate_tests[i];
        Slice in =
            slice_from((void *)(uintptr_t)h->in, h->in_len, h->in_len, TYPE_BYTE);
        Slice got = flate_pattern(a, h->level, 0, in);
        Slice want =
            slice_from((void *)(uintptr_t)h->out, h->out_len, h->out_len, TYPE_BYTE);
        if (!bytes_eq(got, want))
            testing_t_errorf_v(t, "%d: Deflate(%d, %x) = %x, want %x", (int)i, h->level,
                               in, got, want);
    }
    arena_free(&ar);
}

static void TestWriterClose(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    Error err;
    FlateWriter *zw = flate_new_writer(a, bytes_buffer_as_io_writer(&b), 6, &err);
    if (zw == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    Int c = flate_writer_write(zw, BS("Test"), &err);
    if (BURROW_FAILED(err) || c != 4)
        testing_t_fatalf_v(t, "Write to not closed writer: %s, %d", error_text(err),
                           (int)c);
    err = flate_writer_close(zw);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Close: %s", error_text(err));
    Int after_close = bytes_buffer_len(&b);
    c = flate_writer_write(zw, BS("Test"), &err);
    if (!BURROW_FAILED(err) || c != 0)
        testing_t_fatalf_v(t, "Write to closed writer: %s, %d", error_text(err),
                           (int)c);
    err = flate_writer_flush(zw);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Flush to closed writer: %s", error_text(err));
    err = flate_writer_close(zw);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Close: %s", error_text(err));
    if (after_close != bytes_buffer_len(&b))
        testing_t_fatalf_v(
            t,
            "Writer wrote data after close. After close: %d. After writes on "
            "closed stream: %d",
            (int)after_close, (int)bytes_buffer_len(&b));
    flate_writer_free(zw);
    arena_free(&ar);
}

/* Go's sparseReader: l bytes, zeros and then 1<<16 ones. */
typedef struct SparseReader {
    int64_t l;
    int64_t cur;
} SparseReader;

static Int sparse_read(void *self, Slice b, Error *err) {
    SparseReader *r = (SparseReader *)self;
    if (r->cur >= r->l) {
        *err = io_eof;
        return 0;
    }
    Int n = b.len;
    int64_t cur = r->cur + n;
    if (cur > r->l) {
        n -= (Int)(cur - r->l);
        cur = r->l;
    }
    for (Int i = 0; i < n; i++)
        bp(b)[i] = r->cur + i >= r->l - (1 << 16) ? 1 : 0;
    r->cur = cur;
    *err = BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT sparse_reader_vt = {NULL, sparse_read};

static void TestVeryLongSparseChunk(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping sparse chunk during short test");
    Error err;
    FlateWriter *w = flate_new_writer(heap_allocator(), io_discard, 1, &err);
    if (w == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    SparseReader sr = {(int64_t)23e8, 0};
    io_copy(heap_allocator(), flate_writer_as_io_writer(w),
            (IoReader){&sparse_reader_vt, &sr}, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Compress failed: %s", error_text(err));
    flate_writer_free(w);
}

/* Go's testSync, without the goroutine: the compressed bytes go into one
 * buffer the reader reads from, and the first half must decode from what the
 * Flush wrote. */
static void test_sync(TestingT *t, Alloc *a, int level, Slice input, const char *name) {
    if (input.len == 0)
        return;
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err;
    FlateWriter *w = flate_new_writer(a, bytes_buffer_as_io_writer(&buf), level, &err);
    IoReadCloser r = flate_new_reader(a, bytes_buffer_as_io_reader(&buf));
    for (int i = 0; i < 2; i++) {
        Int lo, hi;
        if (i == 0)
            lo = 0, hi = (input.len + 1) / 2;
        else
            lo = (input.len + 1) / 2, hi = input.len;
        flate_writer_write(w, slice_sub(input, lo, hi), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "testSync: write: %s", error_text(err));
            return;
        }
        if (i == 0) {
            err = flate_writer_flush(w);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "testSync: flush: %s", error_text(err));
                return;
            }
        } else {
            err = flate_writer_close(w);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "testSync: close: %s", error_text(err));
        }
        Slice out = make_bytes(a, hi - lo + 1);
        Int m = io_read_at_least(io_read_closer_as_io_reader(r), out, hi - lo, &err);
        if (m != hi - lo || BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "testSync/%d (%d, %d, %s): read %d: %d, %s (%d left)",
                               i, level, (int)input.len, name, (int)(hi - lo), (int)m,
                               error_text(err), (int)bytes_buffer_len(&buf));
            return;
        }
        if (memcmp(bp(input) + lo, out.p, (size_t)(hi - lo)) != 0) {
            testing_t_errorf_v(t, "testSync/%d: read wrong bytes", i);
            return;
        }
    }
    Byte ten[10];
    Int n = io_read_closer_as_io_reader(r).vt->read(
        r.data, slice_from(ten, 10, 10, TYPE_BYTE), &err);
    if (n > 0 || !same_error(err, io_eof))
        testing_t_errorf_v(t, "testSync (%d, %d, %s): final Read: %d, %s", level,
                           (int)input.len, name, (int)n, error_text(err));
    if (bytes_buffer_len(&buf) != 0)
        testing_t_errorf_v(t, "testSync (%d, %d, %s): extra data at end", level,
                           (int)input.len, name);
    flate_reader_free(r);
    flate_writer_free(w);
}

static void test_to_from_with_level_and_limit(TestingT *t, Alloc *a, int level,
                                              Slice input, const char *name,
                                              Int limit) {
    Slice c = flate_pattern(a, level, 0, input);
    if (limit > 0 && c.len > limit)
        testing_t_errorf_v(t, "level: %d, len(compress(data)) = %d > limit = %d", level,
                           (int)c.len, (int)limit);
    Error err;
    Slice out = inflate_all(a, c, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "read: %s", error_text(err));
        return;
    }
    if (!bytes_eq(input, out)) {
        testing_t_errorf_v(t, "decompress(compress(data)) != data: level=%d input=%s",
                           level, name);
        return;
    }
    test_sync(t, a, level, input, name);
}

static void TestDeflateInflate(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice ins[6] = {
        no_dict,
        BS("\x11"),
        BS("\x11\x12"),
        BS("\x11\x11\x11\x11\x11\x11\x11\x11"),
        BS("\x11\x10\x13\x41\x21\x21\x41\x13\x87\x78\x13"),
        large_data_chunk(a),
    };
    for (int i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof name, "#%d", i);
        for (int level = 0; level < 10; level++)
            test_to_from_with_level_and_limit(t, a, level, ins[i], name, 0);
        test_to_from_with_level_and_limit(t, a, -2, ins[i], name, 0);
    }
    arena_free(&ar);
}

static void TestReverseBits(TestingT *t) {
    static const struct {
        uint16_t in;
        uint8_t bit_count;
        uint16_t out;
    } tests[] = {{1, 1, 1},  {1, 2, 2},   {1, 3, 4},     {1, 4, 8},
                 {1, 5, 16}, {17, 5, 17}, {257, 9, 257}, {29, 5, 23}};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        uint16_t v = burrow__flate_reverse_bits(tests[i].in, tests[i].bit_count);
        if (v != tests[i].out)
            testing_t_errorf_v(t, "reverseBits(%d,%d) = %d, want %d", tests[i].in,
                               tests[i].bit_count, v, tests[i].out);
    }
}

/* Go's TestDeflateInflateString runs e.txt and Opticks at every level with a
 * size limit for each. The words and the noise stand in for them here, with
 * limits that are Go's sizes for them. */
static void TestDeflateInflateString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int id = 1; id <= 3; id++) {
        Slice in = flate_input(a, id);
        char name[16];
        snprintf(name, sizeof name, "input %d", id);
        for (size_t i = 0; i < sizeof flate_vectors / sizeof flate_vectors[0]; i++) {
            const FlateVector *v = &flate_vectors[i];
            if (v->input == id && v->pattern == 0)
                test_to_from_with_level_and_limit(t, a, v->level, in, name, v->len);
        }
    }
    arena_free(&ar);
}

static void TestReaderDict(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    Error err;
    FlateWriter *w = flate_new_writer(a, bytes_buffer_as_io_writer(&b), 5, &err);
    if (w == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    flate_writer_write(w, BS("hello world"), &err);
    (void)flate_writer_flush(w);
    bytes_buffer_reset(&b);
    flate_writer_write(w, BS("hello again world"), &err);
    (void)flate_writer_close(w);
    Slice data = inflate_dict(a, bytes_buffer_bytes(&b), BS("hello world"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bytes_eq(data, BS("hello again world")))
        testing_t_fatalf_v(t, "read returned %q want %q",
                           str_from_bytes(bp(data), data.len),
                           BURROW_S("hello again world"));
    flate_writer_free(w);
    arena_free(&ar);
}

static void TestWriterDict(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice dict =
        BS("hello world Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed "
           "do eiusmod tempor incididunt ut labore et dolore magna aliqua.");
    Slice text = BS("hello world again Lorem ipsum dolor sit amet");
    for (int l = 4; l < 9; l++) {
        BytesBuffer b = BYTES_BUFFER(a);
        Error err;
        FlateWriter *w = flate_new_writer(a, bytes_buffer_as_io_writer(&b), l, &err);
        if (w == NULL)
            testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
        flate_writer_write(w, dict, &err);
        (void)flate_writer_flush(w);
        bytes_buffer_reset(&b);
        flate_writer_write(w, text, &err);
        (void)flate_writer_close(w);
        flate_writer_free(w);

        BytesBuffer b1 = BYTES_BUFFER(a);
        w = flate_new_writer_dict(a, bytes_buffer_as_io_writer(&b1), l, dict, &err);
        flate_writer_write(w, text, &err);
        (void)flate_writer_close(w);
        flate_writer_free(w);
        if (!bytes_eq(bytes_buffer_bytes(&b1), bytes_buffer_bytes(&b)))
            testing_t_errorf_v(t, "level=%d: writer wrote\n%x\n want\n%x", l,
                               bytes_buffer_bytes(&b1), bytes_buffer_bytes(&b));
    }
    arena_free(&ar);
}

static void TestRegression2508(TestingT *t) {
    if (testing_short()) {
        testing_t_logf_v(t, "test disabled with -short");
        return;
    }
    Error err;
    FlateWriter *w = flate_new_writer(heap_allocator(), io_discard, 1, &err);
    if (w == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    static Byte buf[1024];
    for (int i = 0; i < 131072; i++) {
        flate_writer_write(w, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "writer failed: %s", error_text(err));
    }
    (void)flate_writer_close(w);
    flate_writer_free(w);
}

static void test_reset_output(TestingT *t, Alloc *a, int level, Slice dict) {
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err;
    FlateWriter *w =
        dict.p != NULL
            ? flate_new_writer_dict(a, bytes_buffer_as_io_writer(&buf), level, dict,
                                    &err)
            : flate_new_writer(a, bytes_buffer_as_io_writer(&buf), level, &err);
    if (w == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    Slice b = BS("hello world - how are you doing?");
    for (int i = 0; i < 1024; i++)
        flate_writer_write(w, b, &err);
    (void)flate_writer_close(w);
    Slice out1 = bytes_buffer_bytes(&buf);

    BytesBuffer buf2 = BYTES_BUFFER(a);
    flate_writer_reset(w, bytes_buffer_as_io_writer(&buf2));
    for (int i = 0; i < 1024; i++)
        flate_writer_write(w, b, &err);
    (void)flate_writer_close(w);
    Slice out2 = bytes_buffer_bytes(&buf2);
    flate_writer_free(w);
    if (out1.len != out2.len) {
        testing_t_errorf_v(t, "dict=%d/level=%d: got %d, expected %d bytes",
                           dict.p != NULL, level, (int)out2.len, (int)out1.len);
        return;
    }
    if (!bytes_eq(out1, out2))
        testing_t_errorf_v(t, "dict=%d/level=%d: output differs after Reset",
                           dict.p != NULL, level);
}

static void TestWriterReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int level = -2; level <= 9; level++) {
        if (level == -1)
            level++;
        if (testing_short() && level > 1)
            break;
        Error err;
        FlateWriter *w = flate_new_writer(a, io_discard, level, &err);
        if (w == NULL)
            testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
        int n = testing_short() ? 10 : 1024;
        for (int i = 0; i < n; i++)
            flate_writer_write(w, BS("hello world"), &err);
        flate_writer_reset(w, io_discard);
        FlateWriter *wref = flate_new_writer(a, io_discard, level, &err);
        if (!burrow__flate_writer_state_eq(w, wref))
            testing_t_errorf_v(t, "level %d Writer not reset after Reset", level);
        flate_writer_free(w);
        flate_writer_free(wref);
    }
    for (int i = FLATE_HUFFMAN_ONLY; i <= FLATE_BEST_COMPRESSION; i++)
        test_reset_output(t, a, i, no_dict);
    for (int i = FLATE_HUFFMAN_ONLY; i <= FLATE_BEST_COMPRESSION; i++)
        test_reset_output(t, a, i, BS(small_dict_text));
    arena_free(&ar);
}

static void TestBestSpeed(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice abcabc = make_bytes(a, 131072);
    for (Int i = 0; i < abcabc.len; i++)
        bp(abcabc)[i] = (Byte)(i % 128);
    static int test_cases[][4] = {
        {3, 65536, 0},          {2, 65536, 1},
        {3, 65536, 1, 256},     {3, 65536, 1, 65536},
        {2, 65536, 14},         {2, 65536, 15},
        {2, 65536, 16},         {3, 65536, 16, 256},
        {3, 65536, 16, 65536},  {2, 65536, 127},
        {2, 65536, 128},        {3, 65536, 128, 256},
        {3, 65536, 128, 65536}, {2, 65536, 129},
        {3, 65536, 65536, 256}, {3, 65536, 65536, 65536},
    };
    test_cases[0][0] = 2;
    static const int first_ns[] = {1, 65534, 65535, 65536, 65537, 131072};
    for (size_t i = 0; i < sizeof test_cases / sizeof test_cases[0]; i++) {
        if (testing_short() && i >= 6)
            break;
        int *tc = test_cases[i];
        for (size_t f = 0; f < sizeof first_ns / sizeof first_ns[0]; f++) {
            tc[1] = first_ns[f];
            for (int flush = 0; flush <= 1; flush++) {
                BytesBuffer buf = BYTES_BUFFER(a);
                BytesBuffer want = BYTES_BUFFER(a);
                Error err;
                FlateWriter *w = flate_new_writer(a, bytes_buffer_as_io_writer(&buf),
                                                  FLATE_BEST_SPEED, &err);
                for (int k = 1; k <= tc[0]; k++) {
                    Slice p = slice_sub(abcabc, 0, tc[k]);
                    bytes_buffer_write(&want, p, NULL);
                    flate_writer_write(w, p, &err);
                    if (BURROW_FAILED(err))
                        testing_t_errorf_v(t, "i=%d, firstN=%d, flush=%d: Write: %s",
                                           (int)i, first_ns[f], flush, error_text(err));
                    if (flush) {
                        err = flate_writer_flush(w);
                        if (BURROW_FAILED(err))
                            testing_t_errorf_v(
                                t, "i=%d, firstN=%d, flush=%d: Flush: %s", (int)i,
                                first_ns[f], flush, error_text(err));
                    }
                }
                err = flate_writer_close(w);
                if (BURROW_FAILED(err))
                    testing_t_errorf_v(t, "i=%d, firstN=%d, flush=%d: Close: %s",
                                       (int)i, first_ns[f], flush, error_text(err));
                flate_writer_free(w);
                Slice got = inflate_all(a, bytes_buffer_bytes(&buf), &err);
                if (BURROW_FAILED(err))
                    testing_t_errorf_v(t, "i=%d, firstN=%d, flush=%d: ReadAll: %s",
                                       (int)i, first_ns[f], flush, error_text(err));
                else if (!bytes_eq(got, bytes_buffer_bytes(&want)))
                    testing_t_errorf_v(t,
                                       "i=%d, firstN=%d, flush=%d: corruption during "
                                       "deflate-then-inflate",
                                       (int)i, first_ns[f], flush);
            }
        }
    }
    arena_free(&ar);
}

static void TestWriterPersistentWriteError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice d = flate_words(a, 10000);
    Error err;
    FlateWriter *zw =
        flate_new_writer(a, (IoWriter){NULL, NULL}, FLATE_DEFAULT_COMPRESSION, &err);
    if (zw == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    for (int i = 0; i < 1000; i++) {
        FailWriter fw = {i};
        flate_writer_reset(zw, (IoWriter){&fail_writer_vt, &fw});
        Error werr;
        flate_writer_write(zw, d, &werr);
        Error cerr = flate_writer_close(zw);
        Error ferr = flate_writer_flush(zw);
        if (!same_error(werr, err_io) && BURROW_FAILED(werr))
            testing_t_errorf_v(t, "test %d, mismatching Write error: got %s, want %s",
                               i, error_text(werr), error_text(err_io));
        if (!same_error(cerr, err_io) && fw.n < 0)
            testing_t_errorf_v(t, "test %d, mismatching Close error: got %s, want %s",
                               i, error_text(cerr), error_text(err_io));
        if (!same_error(ferr, err_io) && fw.n < 0)
            testing_t_errorf_v(t, "test %d, mismatching Flush error: got %s, want %s",
                               i, error_text(ferr), error_text(err_io));
        if (fw.n >= 0)
            break;
    }
    flate_writer_free(zw);
    arena_free(&ar);
}

static void check_errors(TestingT *t, const Error *got, int n, Error want) {
    for (int i = 0; i < n; i++)
        if (!same_error(got[i], want))
            testing_t_errorf_v(t, "Error doesn't match\nWant: %s\nGot: %s",
                               error_text(want), error_text(got[i]));
}

static void TestWriterPersistentFlushError(TestingT *t) {
    FailWriter fw = {0};
    Error err;
    FlateWriter *zw =
        flate_new_writer(heap_allocator(), (IoWriter){&fail_writer_vt, &fw},
                         FLATE_DEFAULT_COMPRESSION, &err);
    if (zw == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    Error errs[3];
    errs[1] = flate_writer_flush(zw);
    errs[0] = flate_writer_close(zw);
    flate_writer_write(zw, BS("Test"), &errs[2]);
    check_errors(t, errs, 3, err_io);
    flate_writer_free(zw);
}

static void TestWriterPersistentCloseError(TestingT *t) {
    FailWriter fw = {0};
    Error err;
    FlateWriter *zw =
        flate_new_writer(heap_allocator(), (IoWriter){&fail_writer_vt, &fw},
                         FLATE_DEFAULT_COMPRESSION, &err);
    if (zw == NULL)
        testing_t_fatalf_v(t, "NewWriter: %s", error_text(err));
    Error errs[3];
    errs[0] = flate_writer_close(zw);
    errs[1] = flate_writer_flush(zw);
    flate_writer_write(zw, BS("Test"), &errs[2]);
    check_errors(t, errs, 3, err_io);

    BytesBuffer b = BYTES_BUFFER(heap_allocator());
    flate_writer_reset(zw, bytes_buffer_as_io_writer(&b));
    err = flate_writer_close(zw);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "First call to close returned error: %s",
                           error_text(err));
    err = flate_writer_close(zw);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Second call to close returned error: %s",
                           error_text(err));
    errs[0] = flate_writer_flush(zw);
    flate_writer_write(zw, BS("Test"), &errs[1]);
    check_errors(t, errs, 2, burrow__flate_err_writer_closed());
    CHECK(text_is(error_text(errs[0]), "flate: closed writer"));
    bytes_buffer_free(&b);
    flate_writer_free(zw);
}

static void TestBestSpeedMatch(TestingT *t) {
    static const Byte zeros[1000];
    static const Byte p00012[] = {0, 0, 0, 1, 2}, p00011[] = {0, 0, 0, 1, 1};
    static const Byte p0001234522[] = {0, 0, 0, 1, 2, 3, 4, 5, 2, 2};
    static const Byte p99999[] = {9, 9, 9, 9, 9}, p345[] = {3, 4, 5};
    static const Byte c345012345[] = {3, 4, 5, 0, 1, 2, 3, 4, 5};
    static const Byte c245012345[] = {2, 4, 5, 0, 1, 2, 3, 4, 5};
    static const Byte c222212345[] = {2, 2, 2, 2, 1, 2, 3, 4, 5};
    static const Byte c922212345[] = {9, 2, 2, 2, 1, 2, 3, 4, 5};
    const struct {
        const Byte *previous;
        Int plen;
        const Byte *current;
        Int clen;
        int t, s;
        int32_t want;
    } cases[] = {
        {p00012, 5, c345012345, 9, -3, 3, 6},
        {p00012, 5, c245012345, 9, -3, 3, 3},
        {p00011, 5, c345012345, 9, -3, 3, 2},
        {p00012, 5, c222212345, 9, -1, 0, 4},
        {p0001234522, 10, c222212345, 9, -7, 4, 5},
        {p99999, 5, c222212345, 9, -1, 0, 0},
        {p99999, 5, c922212345, 9, 0, 1, 0},
        {NULL, 0, c222212345, 9, 0, 1, 3},
        {p345, 3, p345, 3, -3, 0, 3},
        {zeros, 1000, zeros, 1000, -1000, 0, 258 - 4},
        {zeros, 200, zeros, 500, -200, 0, 258 - 4},
        {zeros, 200, zeros, 500, 0, 1, 258 - 4},
        {zeros, 258 - 4, zeros, 500, -(258 - 4), 0, 258 - 4},
        {zeros, 200, zeros, 500, -200, 400, 100},
        {zeros, 10, zeros, 500, 200, 400, 100},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        BurrowFlateFast *e = burrow__flate_fast_new(1);
        burrow__flate_fast_add_block(e, cases[i].previous, cases[i].plen);
        burrow__flate_fast_add_block(e, cases[i].current, cases[i].clen);
        int32_t got = burrow__flate_fast_match_len_limited(
            e, (int32_t)(cases[i].s + cases[i].plen),
            (int32_t)(cases[i].t + cases[i].plen));
        if (got != cases[i].want)
            testing_t_errorf_v(t, "Test %d: match length, want %d, got %d", (int)i,
                               cases[i].want, got);
        burrow__flate_fast_free(e);
    }
}

static void TestBestSpeedMaxMatchOffset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const char abc[] = "abcdefgh", xyz[] = "stuvwxyz";
    enum { input_margin = 16 - 1 };
    static const int extras[] = {0, input_margin - 1, input_margin, input_margin + 1,
                                 2 * input_margin};
    for (int match_before = 0; match_before <= 1; match_before++) {
        for (size_t e = 0; e < sizeof extras / sizeof extras[0]; e++) {
            for (int offset_adj = -5; offset_adj <= 5; offset_adj++) {
                Int offset = 32768 + offset_adj;
                Slice src = make_bytes(a, offset + 8 + extras[e]);
                memset(src.p, 0, (size_t)src.len);
                memcpy(src.p, abc, 8);
                if (!match_before)
                    memcpy(bp(src) + offset - 8, xyz, 8);
                memcpy(bp(src) + offset, abc, 8);
                Slice c = flate_pattern(a, FLATE_BEST_SPEED, 0, src);
                Error err;
                Slice dst = inflate_all(a, c, &err);
                if (BURROW_FAILED(err))
                    testing_t_errorf_v(
                        t, "matchBefore=%d, extra=%d, offsetAdj=%d: ReadAll: %s",
                        match_before, extras[e], offset_adj, error_text(err));
                else if (!bytes_eq(dst, src))
                    testing_t_errorf_v(
                        t,
                        "matchBefore=%d, extra=%d, offsetAdj=%d: bytes differ "
                        "after round-tripping",
                        match_before, extras[e], offset_adj);
            }
        }
    }
    arena_free(&ar);
}

static uint16_t val_or_len(Int n, Int len) {
    return n == 0 ? (uint16_t)len : (uint16_t)n;
}

static void TestBestSpeedShiftOffsets(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte test_data[100];
    MathRandRand *rng = math_rand_new(a, math_rand_new_source(a, 0));
    for (int i = 0; i < 100; i++)
        test_data[i] = (Byte)math_rand_rand_uint32(rng);
    static uint32_t toks[256];
    const int32_t buffer_reset = burrow__flate_fast_buffer_reset();
    for (int level = 1; level <= 6; level++) {
        BurrowFlateFast *enc = burrow__flate_fast_new(level);
        uint16_t want_first =
            val_or_len(burrow__flate_fast_encode(enc, test_data, 100, toks, 256), 100);
        uint16_t want_second =
            val_or_len(burrow__flate_fast_encode(enc, test_data, 100, toks, 256), 100);
        if (want_first <= want_second)
            testing_t_fatalf_v(
                t,
                "level=%d: test needs matches between inputs to be generated, %d == %d",
                level, want_first, want_second);
        burrow__flate_fast_drop_hist(enc);
        burrow__flate_fast_set_cur(enc, buffer_reset - 100);

        uint16_t got =
            val_or_len(burrow__flate_fast_encode(enc, test_data, 100, toks, 256), 100);
        if (want_first != got)
            testing_t_errorf_v(t, "level=%d: got %d, want %d tokens", level, got,
                               want_first);
        int64_t got_cur =
            (int64_t)burrow__flate_fast_cur(enc) + burrow__flate_fast_hist_len(enc);
        if (got_cur != buffer_reset)
            testing_t_errorf_v(t,
                               "level=%d: got %d, want e.cur to be at bufferReset (%d)",
                               level, (int)got_cur, buffer_reset);

        got =
            val_or_len(burrow__flate_fast_encode(enc, test_data, 100, toks, 256), 100);
        if (want_second != got)
            testing_t_errorf_v(t, "level=%d: got %d, want %d token", level, got,
                               want_second);
        if (burrow__flate_fast_cur(enc) >= buffer_reset)
            testing_t_errorf_v(t,
                               "level=%d: want e.cur to be < bufferReset (%d), got %d",
                               level, buffer_reset, burrow__flate_fast_cur(enc));

        burrow__flate_fast_set_cur(enc, buffer_reset);
        burrow__flate_fast_drop_hist(enc);
        got =
            val_or_len(burrow__flate_fast_encode(enc, test_data, 100, toks, 256), 100);
        if (want_first != got)
            testing_t_errorf_v(t, "level=%d: got %d, want %d tokens", level, got,
                               want_first);
        burrow__flate_fast_free(enc);
    }
    arena_free(&ar);
}

/* -------------------------------------------------------- writer_test.go */

static void TestWriteError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = asd_lines(a, testing_short() ? 65536 : 65536 * 4, false);
    Byte copy_buffer[128];
    for (int l = 0; l < 10; l++) {
        for (int fail = 1; fail <= 256; fail *= 2) {
            ErrorWriter ew = {fail};
            Error err;
            FlateWriter *w =
                flate_new_writer(a, (IoWriter){&error_writer_vt, &ew}, l, &err);
            if (w == NULL)
                testing_t_fatalf_v(t, "NewWriter: level %d: %s", l, error_text(err));
            BytesReader br;
            bytes_reader_reset(&br, in);
            PlainReader pr = {bytes_reader_as_io_reader(&br)};
            io_copy_buffer(flate_writer_as_io_writer(w),
                           (IoReader){&plain_reader_vt, &pr},
                           slice_from(copy_buffer, 128, 128, TYPE_BYTE), &err);
            if (!BURROW_FAILED(err))
                testing_t_fatalf_v(t, "Level %d: Expected an error", l);
            Int n2 = flate_writer_write(w, BS("\x01\x02\x02\x03\x04\x05"), &err);
            if (n2 != 0)
                testing_t_fatalf_v(t, "Level %d Expected 0 length write, got %d", l,
                                   (int)n2);
            if (!BURROW_FAILED(err))
                testing_t_fatalf_v(t, "Level %d Expected an error", l);
            if (!BURROW_FAILED(flate_writer_flush(w)))
                testing_t_fatalf_v(t, "Level %d Expected an error on flush", l);
            if (!BURROW_FAILED(flate_writer_close(w)))
                testing_t_fatalf_v(t, "Level %d Expected an error on close", l);
            flate_writer_reset(w, io_discard);
            n2 = flate_writer_write(w, BS("\x01\x02\x03\x04\x05\x06"), &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "Level %d Got unexpected error after reset: %s",
                                   l, error_text(err));
            if (n2 == 0)
                testing_t_fatalf_v(t, "Level %d Got 0 length write, expected > 0", l);
            flate_writer_free(w);
            if (testing_short())
                goto done;
        }
    }
done:
    arena_free(&ar);
}

static void TestWriter_Reset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = asd_lines(a, testing_short() ? 65536 : 65536 * 4, false);
    const Int buffer_reset = burrow__flate_fast_buffer_reset();
    for (int l = 0; l < 10; l++) {
        if (testing_short() && l > 1)
            break;
        for (int offset = testing_short() ? 256 : 1; offset <= 256; offset *= 2) {
            Error err;
            FlateWriter *w = flate_new_writer(a, io_discard, l, &err);
            if (w == NULL)
                testing_t_fatalf_v(t, "NewWriter: level %d: %s", l, error_text(err));
            if (!burrow__flate_writer_fast_reset(w)) {
                flate_writer_free(w);
                break;
            }
            for (Int i = 1; i < (buffer_reset - in.len - offset - 32768) / 32768; i++)
                burrow__flate_writer_fast_reset(w);
            burrow__flate_writer_fast_reset(w);
            flate_writer_write(w, in, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "level=%d: %s", l, error_text(err));
            for (int i = 0; i < 50; i++)
                burrow__flate_writer_fast_reset(w);
            burrow__flate_writer_fast_reset(w);
            flate_writer_write(w, in, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "level=%d: %s", l, error_text(err));
            for (int64_t i = 0; i < ((int64_t)UINT32_MAX - buffer_reset) / 32768; i++)
                burrow__flate_writer_fast_reset(w);
            flate_writer_write(w, in, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "level=%d: %s", l, error_text(err));
            err = flate_writer_close(w);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "level=%d: %s", l, error_text(err));
            flate_writer_free(w);
        }
    }
    arena_free(&ar);
}

/* Copies in to w through a reader with only Read, buf at a time, Go's
 * io.CopyBuffer(w, struct{ io.Reader }{bytes.NewBuffer(in)}, buf). */
static void copy_plain(FlateWriter *w, Slice in, Slice buf) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    PlainReader pr = {bytes_reader_as_io_reader(&br)};
    Error err;
    io_copy_buffer(flate_writer_as_io_writer(w), (IoReader){&plain_reader_vt, &pr}, buf,
                   &err);
}

static void test_deterministic(TestingT *t, Alloc *a, int i) {
    Int length = 65535 * 30 + 500;
    if (testing_short())
        length /= 10;
    MathRandRand *rng = math_rand_new(a, math_rand_new_source(a, 1));
    Slice t1 = make_bytes(a, length);
    for (Int k = 0; k < length; k++)
        bp(t1)[k] = (Byte)(math_rand_rand_int63(rng) & 7);

    Error err;
    BytesBuffer b1 = BYTES_BUFFER(a);
    FlateWriter *w = flate_new_writer(a, bytes_buffer_as_io_writer(&b1), i, &err);
    copy_plain(w, t1, make_bytes(a, 787));
    (void)flate_writer_close(w);
    flate_writer_free(w);

    BytesBuffer b2 = BYTES_BUFFER(a);
    w = flate_new_writer(a, bytes_buffer_as_io_writer(&b2), i, &err);
    copy_plain(w, t1, make_bytes(a, 81761));
    (void)flate_writer_close(w);
    flate_writer_free(w);

    Slice b1b = bytes_buffer_bytes(&b1), b2b = bytes_buffer_bytes(&b2);
    if (!bytes_eq(b1b, b2b))
        testing_t_errorf_v(
            t,
            "level %d did not produce deterministic result, result mismatch, "
            "len(a) = %d, len(b) = %d",
            i, (int)b1b.len, (int)b2b.len);

    BytesBuffer b3 = BYTES_BUFFER(a);
    BytesBuffer br = BYTES_BUFFER(a);
    bytes_buffer_write(&br, t1, NULL);
    w = flate_new_writer(a, bytes_buffer_as_io_writer(&b3), i, &err);
    bytes_buffer_write_to(&br, flate_writer_as_io_writer(w), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    (void)flate_writer_close(w);
    flate_writer_free(w);
    if (!bytes_eq(b1b, bytes_buffer_bytes(&b3)))
        testing_t_errorf_v(
            t,
            "level %d (io.WriterTo) did not produce deterministic result, result "
            "mismatch, len(a) = %d, len(b) = %d",
            i, (int)b1b.len, (int)bytes_buffer_len(&b3));
}

static void TestDeterministic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i <= 9; i++)
        test_deterministic(t, a, i);
    test_deterministic(t, a, -2);
    arena_free(&ar);
}

static void TestDeflateFast_Reset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = asd_lines(a, 65536, true);
    const int level = 1;
    BytesBuffer want = BYTES_BUFFER(a);
    Error err;
    FlateWriter *w = flate_new_writer(a, bytes_buffer_as_io_writer(&want), level, &err);
    for (int i = 0; i < 3; i++)
        flate_writer_write(w, in, &err);
    (void)flate_writer_close(w);
    flate_writer_free(w);
    const Int buffer_reset = burrow__flate_fast_buffer_reset();
    for (int offset = testing_short() ? 256 : 1; offset <= 256; offset *= 2) {
        w = flate_new_writer(a, io_discard, level, &err);
        for (Int i = 0; i < (buffer_reset - in.len - offset - 32768) / 32768; i++)
            burrow__flate_writer_reset_nil(w);
        BytesBuffer got = BYTES_BUFFER(a);
        flate_writer_reset(w, bytes_buffer_as_io_writer(&got));
        for (int i = 0; i < 3; i++) {
            flate_writer_write(w, in, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
        }
        err = flate_writer_close(w);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        flate_writer_free(w);
        if (!bytes_eq(bytes_buffer_bytes(&got), bytes_buffer_bytes(&want)))
            testing_t_fatalf_v(
                t,
                "output did not match at wraparound, len(want)  = %d, len(got) "
                "= %d",
                (int)bytes_buffer_len(&want), (int)bytes_buffer_len(&got));
    }
    arena_free(&ar);
}

/* ------------------------------------------ huffman_bit_writer_test.go */

static bool matches(Slice got, Int want_len, const char *want_sha) {
    char hex[65];
    sha_hex(got, hex);
    return got.len == want_len && strcmp(hex, want_sha) == 0;
}

static void TestBlockHuff(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof huff_goldens / sizeof huff_goldens[0]; i++) {
        const HuffGolden *g = &huff_goldens[i];
        Slice all = gen_bytes(a, g->input);
        BytesBuffer buf = BYTES_BUFFER(a);
        BurrowFlateBitWriter *bw =
            burrow__flate_bw_new(bytes_buffer_as_io_writer(&buf));
        burrow__flate_bw_write_block_huff(bw, false, bp(all), all.len, false);
        burrow__flate_bw_flush(bw);
        burrow__flate_bw_free(bw);
        if (!matches(bytes_buffer_bytes(&buf), g->len, g->sha256))
            testing_t_errorf_v(t, "%s: got %d bytes that are not the golden %d",
                               g->name, (int)bytes_buffer_len(&buf), (int)g->len);
    }
    arena_free(&ar);
}

static const char *const huff_types[3] = {"wb", "dyn", "sync"};

static void write_to_type(TestingT *t, int ttype, BurrowFlateBitWriter *bw,
                          const HuffTest *h, const Byte *input, Int input_len) {
    if (ttype == 0)
        burrow__flate_bw_write_block(bw, h->tokens, h->ntokens, false, input,
                                     input_len);
    else
        burrow__flate_bw_write_block_dynamic(bw, h->tokens, h->ntokens, false, input,
                                             input_len, ttype == 2);
    Error err = burrow__flate_bw_err(bw);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s", error_text(err));
        return;
    }
    burrow__flate_bw_flush(bw);
    err = burrow__flate_bw_err(bw);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
}

/* Go's testWriterEOF, which always writes the "wb" way with eof set. */
static void test_writer_eof(TestingT *t, Alloc *a, const HuffTest *h, bool use_input) {
    if (use_input && h->input == NULL)
        return;
    Slice input = use_input ? gen_bytes(a, h->input) : no_dict;
    BytesBuffer buf = BYTES_BUFFER(a);
    BurrowFlateBitWriter *bw = burrow__flate_bw_new(bytes_buffer_as_io_writer(&buf));
    burrow__flate_bw_write_block(bw, h->tokens, h->ntokens, true, bp(input), input.len);
    burrow__flate_bw_flush(bw);
    burrow__flate_bw_free(bw);
    Slice b = bytes_buffer_bytes(&buf);
    if (b.len == 0)
        testing_t_errorf_v(t, "no output received");
    else if ((bp(b)[0] & 1) != 1)
        testing_t_errorf_v(t, "block not marked with EOF for input %s", h->name);
}

static void test_block(TestingT *t, const HuffTest *h, int ttype) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int with_input = 1; with_input >= 0; with_input--) {
        if (with_input && h->input == NULL)
            continue;
        Slice input = with_input ? gen_bytes(a, h->input) : no_dict;
        Int want_len = with_input ? h->want_len[ttype] : h->want_noinput_len[ttype];
        const char *want = with_input ? h->want[ttype] : h->want_noinput[ttype];
        BytesBuffer buf = BYTES_BUFFER(a);
        BurrowFlateBitWriter *bw =
            burrow__flate_bw_new(bytes_buffer_as_io_writer(&buf));
        write_to_type(t, ttype, bw, h, bp(input), input.len);
        Slice got = bytes_buffer_bytes(&buf);
        if (!matches(got, want_len, want))
            testing_t_errorf_v(
                t, "%s.%s: writeBlock did not yield expected result (%s input)",
                h->name, huff_types[ttype], with_input ? "with" : "without");
        else if (!with_input && (bp(got)[0] & 1) == 1)
            testing_t_errorf_v(t, "%s.%s: got unexpected EOF", h->name,
                               huff_types[ttype]);

        BytesBuffer buf2 = BYTES_BUFFER(a);
        burrow__flate_bw_free(bw);
        bw = burrow__flate_bw_new(bytes_buffer_as_io_writer(&buf2));
        write_to_type(t, ttype, bw, h, bp(input), input.len);
        burrow__flate_bw_flush(bw);
        burrow__flate_bw_free(bw);
        if (!matches(bytes_buffer_bytes(&buf2), want_len, want))
            testing_t_errorf_v(t,
                               "%s.%s: reset: writeBlock did not yield expected result",
                               h->name, huff_types[ttype]);
        test_writer_eof(t, a, h, with_input);
    }
    arena_free(&ar);
}

static void TestWriteBlock(TestingT *t) {
    for (size_t i = 0; i < sizeof huff_tests / sizeof huff_tests[0]; i++)
        test_block(t, &huff_tests[i], 0);
}

static void TestWriteBlockDynamic(TestingT *t) {
    for (size_t i = 0; i < sizeof huff_tests / sizeof huff_tests[0]; i++)
        test_block(t, &huff_tests[i], 1);
}

static void TestWriteBlockDynamicSync(TestingT *t) {
    for (size_t i = 0; i < sizeof huff_tests / sizeof huff_tests[0]; i++)
        test_block(t, &huff_tests[i], 2);
}

/* ------------------------------------------------------------ fuzz_test.go */

/* FuzzEncoding's round trip and determinism check, on the generated inputs
 * and on every prefix length up to 300 of the words. */
static void fuzz_one(TestingT *t, Alloc *a, FlateWriter **encs, Slice data) {
    for (int level = -2; level <= 9; level++) {
        if (level == -1)
            continue;
        FlateWriter *fw = encs[level + 2];
        Slice c[2];
        for (int k = 0; k < 2; k++) {
            BytesBuffer buf = BYTES_BUFFER(a);
            flate_writer_reset(fw, bytes_buffer_as_io_writer(&buf));
            Error err;
            Int n = flate_writer_write(fw, data, &err);
            if (n != data.len)
                testing_t_fatalf_v(t, "level %d: short write", level);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "level %d: %s", level, error_text(err));
            err = flate_writer_close(fw);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "level %d: %s", level, error_text(err));
            c[k] = bytes_buffer_bytes(&buf);
            Slice data2 = inflate_all(a, c[k], &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "level %d: %s", level, error_text(err));
            if (!bytes_eq(data, data2))
                testing_t_fatalf_v(t, "level %d%s: decompressed not equal", level,
                                   k ? " (reset)" : "");
        }
        if (!bytes_eq(c[0], c[1]))
            testing_t_fatalf_v(t, "level %d (reset): non-deterministic output", level);
    }
}

static void TestFuzzEncodingSeeds(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FlateWriter *encs[12];
    for (int i = 0; i < 12; i++) {
        Error err;
        encs[i] = flate_new_writer(a, (IoWriter){NULL, NULL}, i - 2, &err);
    }
    for (int id = 0; id <= 10; id++)
        fuzz_one(t, a, encs, flate_input(a, id));
    Slice words = flate_words(a, 300);
    Slice nz = noise(a, 300, 11, 0x0f);
    for (Int n = 0; n <= 300; n++) {
        fuzz_one(t, a, encs, slice_sub(words, 0, n));
        fuzz_one(t, a, encs, slice_sub(nz, 0, n));
    }
    for (int i = 0; i < 12; i++)
        flate_writer_free(encs[i]);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ new */

/* The streams Go's writer made for the reader tests come out of this writer
 * byte for byte. */
static void TestGoStreamsExact(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice gb = gen_bytes(a, gettysburg);
    Slice words = flate_words(a, 34000);
    for (size_t i = 0; i < sizeof flate_streams / sizeof flate_streams[0]; i++) {
        const FlateStream *s = &flate_streams[i];
        Slice want = gen_bytes(a, s->b64);
        Slice in = s->input == 0   ? gb
                   : s->input == 1 ? words
                                   : slice_sub(words, 5000, words.len);
        Slice dict = s->input == 2 ? slice_sub(words, 0, 5000) : no_dict;
        Slice got = deflate_all(a, s->level, dict, false, in);
        if (!bytes_eq(got, want))
            testing_t_errorf_v(t,
                               "stream %d (level %d, input %d): got %d bytes, want %d",
                               (int)i, s->level, s->input, (int)got.len, (int)want.len);
    }
    Slice data = slice_from(mem_alloc(a, 131072, 1), 131072, 131072, TYPE_BYTE);
    for (Int i = 0; i < data.len; i++)
        ((Byte *)data.p)[i] = (Byte)i;
    for (size_t i = 0; i < sizeof flate_early / sizeof flate_early[0]; i++) {
        const FlateEarly *e = &flate_early[i];
        Slice in = slice_sub(data, 0, e->size);
        if (!bytes_eq(deflate_all(a, 5, no_dict, true, in), gen_bytes(a, e->flushed)))
            testing_t_errorf_v(t, "early %d flushed: differs from Go", (int)e->size);
        if (!bytes_eq(deflate_all(a, 5, no_dict, false, in), gen_bytes(a, e->plain)))
            testing_t_errorf_v(t, "early %d: differs from Go", (int)e->size);
    }
    arena_free(&ar);
}

/* Every generated vector: what Go's writer made of each input at each level,
 * written in one go, in small and large pieces, with flushes, after a
 * dictionary, and a byte at a time. */
static void TestGoVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    int last = -1;
    Slice in = no_dict;
    for (size_t i = 0; i < sizeof flate_vectors / sizeof flate_vectors[0]; i++) {
        const FlateVector *v = &flate_vectors[i];
        if (v->input != last) {
            in = flate_input(a, v->input);
            last = v->input;
        }
        Slice got = flate_pattern(a, v->level, v->pattern, in);
        if (!matches(got, v->len, v->sha256))
            testing_t_errorf_v(
                t, "input %d, level %d, pattern %d: got %d bytes, Go made %d", v->input,
                v->level, v->pattern, (int)got.len, (int)v->len);
    }
    arena_free(&ar);
}

static void TestInvalidLevel(TestingT *t) {
    static const Int levels[] = {-3, 10, 100, -2147483647 - 1};
    static const char *const want[] = {
        "flate: invalid compression level -3: want value in range [-2, 9]",
        "flate: invalid compression level 10: want value in range [-2, 9]",
        "flate: invalid compression level 100: want value in range [-2, 9]",
        "flate: invalid compression level -2147483648: want value in range [-2, 9]",
    };
    for (int i = 0; i < 4; i++) {
        Error err;
        FlateWriter *w =
            flate_new_writer(heap_allocator(), io_discard, levels[i], &err);
        CHECK(w == NULL);
        CHECK(text_is(error_text(err), want[i]));
        w = flate_new_writer_dict(heap_allocator(), io_discard, levels[i], BS("dict"),
                                  &err);
        CHECK(w == NULL);
        CHECK(text_is(error_text(err), want[i]));
    }
}

/* The writer as an IoWriter and an IoWriteCloser, and freeing NULL. */
static void TestWriterInterfaces(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err;
    FlateWriter *w = flate_new_writer(a, bytes_buffer_as_io_writer(&buf), 6, &err);
    IoWriteCloser wc = flate_writer_as_io_write_closer(w);
    IoWriter iw = io_write_closer_as_io_writer(wc);
    CHECK_INT_EQ(iw.vt->write(iw.data, BS("hello, hello, hello, hello\n"), &err), 27);
    CHECK(!BURROW_FAILED(wc.vt->closer.close(wc.data)));
    CHECK(
        bytes_eq(bytes_buffer_bytes(&buf), flate_pattern(a, 6, 0, flate_input(a, 10))));
    CHECK(flate_writer_as_io_writer(w).data == w);
    flate_writer_free(w);
    flate_writer_free(NULL);
    arena_free(&ar);
}

/* Running out of memory at any point in making a writer gives NULL and
 * burrow_err_out_of_memory and leaks nothing. */
static void TestWriterNoMemory(TestingT *t) {
    (void)t;
    static unsigned char room[1 << 16];
    for (int level = -2; level <= 9; level++) {
        Fixed fx;
        fixed_init(&fx, room, sizeof room);
        Error err;
        FlateWriter *w =
            flate_new_writer(fixed_allocator(&fx), io_discard, level, &err);
        CHECK(w == NULL);
        CHECK(same_error(err, burrow_err_out_of_memory));
    }
}

#define TESTS(X)                                                                       \
    X(TestBulkHash4)                                                                   \
    X(TestDeflate)                                                                     \
    X(TestWriterClose)                                                                 \
    X(TestVeryLongSparseChunk)                                                         \
    X(TestDeflateInflate)                                                              \
    X(TestReverseBits)                                                                 \
    X(TestDeflateInflateString)                                                        \
    X(TestReaderDict)                                                                  \
    X(TestWriterDict)                                                                  \
    X(TestRegression2508)                                                              \
    X(TestWriterReset)                                                                 \
    X(TestBestSpeed)                                                                   \
    X(TestWriterPersistentWriteError)                                                  \
    X(TestWriterPersistentFlushError)                                                  \
    X(TestWriterPersistentCloseError)                                                  \
    X(TestBestSpeedMatch)                                                              \
    X(TestBestSpeedMaxMatchOffset)                                                     \
    X(TestBestSpeedShiftOffsets)                                                       \
    X(TestWriteError)                                                                  \
    X(TestWriter_Reset)                                                                \
    X(TestDeterministic)                                                               \
    X(TestDeflateFast_Reset)                                                           \
    X(TestBlockHuff)                                                                   \
    X(TestWriteBlock)                                                                  \
    X(TestWriteBlockDynamic)                                                           \
    X(TestWriteBlockDynamicSync)                                                       \
    X(TestFuzzEncodingSeeds)                                                           \
    X(TestGoStreamsExact)                                                              \
    X(TestGoVectors)                                                                   \
    X(TestInvalidLevel)                                                                \
    X(TestWriterInterfaces)                                                            \
    X(TestWriterNoMemory)

TESTING_MAIN(TESTS)
