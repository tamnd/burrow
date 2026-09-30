/* Derived from Go's src/compress/gzip/gunzip_test.go, gzip_test.go and
 * example_test.go.
 * Go source: go1.27.1.
 *
 * gunzipTests and testdata/issue6550.gz.base64 come over through
 * tests/gzip_test_gen.h, which tools/gen-gzip-tests.sh copies out of Go's own
 * test file, along with the SHA-256 of what Go's writer makes of a set of
 * inputs at every level, so the output is checked byte for byte as well as by
 * reading it back.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/compress/gzip.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/base64.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "flate_test_gen.h"
#include "gzip_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Slice no_bytes = {NULL, 0, 0, NULL};

#define BS(lit)                                                                        \
    slice_from((void *)(uintptr_t)(lit), (Int)sizeof(lit) - 1, (Int)sizeof(lit) - 1,   \
               TYPE_BYTE)

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool str_is(Str s, const char *want) {
    return (size_t)s.len == strlen(want) &&
           (s.len == 0 || memcmp(s.p, want, (size_t)s.len) == 0);
}

static bool bytes_eq(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static Slice const_bytes(const char *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static Slice make_bytes(Alloc *a, Int n) {
    return slice_from(mem_alloc(a, (size_t)n + 1, 1), n, n, TYPE_BYTE);
}

/* Lines of base64, joined and decoded. */
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

/* The words tools/gen-gzip-tests.sh compresses, made the same way. */
static Slice gzip_words(Alloc *a, Int n) {
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

/* Bytes from the same generator, seeded, the top byte of each step. */
static Slice gzip_noise(Alloc *a, Int n, uint64_t seed) {
    Slice b = make_bytes(a, n);
    uint64_t x = seed;
    for (Int i = 0; i < n; i++) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        ((Byte *)b.p)[i] = (Byte)(x >> 56);
    }
    return b;
}

/* The inputs the generator numbers. */
static Slice gzip_input(Alloc *a, int id) {
    switch (id) {
    case 0:
        return gen_bytes(a, gettysburg);
    case 1:
        return gzip_words(a, 34000);
    case 2:
        return gzip_noise(a, 70000, 7);
    case 3:
        return no_bytes;
    default:
        return BS("hello, world\n");
    }
}

/* The headers the generator numbers: what gzip_new_writer starts with, and
 * every field set, with a comment that has to go out as Latin-1. */
static void gzip_set_header(GzipWriter *w, int id) {
    if (id == 0)
        return;
    w->header.comment = BURROW_S("Grüße aus Gettysburg");
    w->header.extra = BS("ex\0tra");
    w->header.mod_time = time_from_unix(1000000000, 0);
    w->header.name = BURROW_S("gettysburg.txt");
    w->header.os = 3;
}

static void sha_hex(Slice b, char out[65]) {
    static const char digits[] = "0123456789abcdef";
    Sha256Sum256Ret h = sha256_sum256(b);
    for (size_t i = 0; i < 32; i++) {
        out[2 * i] = digits[h.a[i] >> 4];
        out[2 * i + 1] = digits[h.a[i] & 15];
    }
    out[64] = 0;
}

/* A reader with only Read, Go's struct{ io.Reader }. */
typedef struct PlainReader {
    IoReader r;
} PlainReader;

static Int plain_read(void *self, Slice p, Error *err) {
    PlainReader *pr = (PlainReader *)self;
    return pr->r.vt->read(pr->r.data, p, err);
}

static const IoReaderVT plain_reader_vt = {NULL, plain_read};

/* Compresses in at level with header h, in one Write and a Close. */
static Slice gzip_all(Alloc *a, Int level, int h, Slice in) {
    BytesBuffer out = BYTES_BUFFER(a);
    Error err;
    GzipWriter *w =
        gzip_new_writer_level(a, bytes_buffer_as_io_writer(&out), level, &err);
    if (w == NULL)
        abort();
    gzip_set_header(w, h);
    Int n = gzip_writer_write(w, in, &err);
    if (n != in.len || BURROW_FAILED(err))
        abort();
    if (BURROW_FAILED(gzip_writer_close(w)))
        abort();
    gzip_writer_free(w);
    return bytes_buffer_bytes(&out);
}

/* io.ReadAll of a gzip reader over in, through a reader with only Read when
 * plain is set. */
static Slice gunzip_all(Alloc *a, Slice in, bool plain, Error *err) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    PlainReader pr = {bytes_reader_as_io_reader(&br)};
    IoReader src = plain ? (IoReader){&plain_reader_vt, &pr} : pr.r;
    GzipReader *zr = gzip_new_reader(a, src, err);
    if (zr == NULL)
        return no_bytes;
    Slice out = io_read_all(a, gzip_reader_as_io_reader(zr), err);
    Error cerr = gzip_reader_close(zr);
    if (BURROW_OK(*err))
        *err = cerr;
    gzip_reader_free(zr);
    return out;
}

static const Error *gz_error(int code) {
    switch (code) {
    case GZ_CHECKSUM:
        return &gzip_err_checksum;
    case GZ_HEADER:
        return &gzip_err_header;
    case GZ_UNEXPECTED_EOF:
        return &io_err_unexpected_eof;
    default:
        return NULL;
    }
}

/* ----------------------------------------------------------- gunzip_test.go */

/* Each case is read through a new reader, then again after a reset, as Go's
 * does, and here both through a source with ReadByte and one without. */
static void TestDecompressor(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gunzip_tests / sizeof gunzip_tests[0]; i++) {
        const GunzipTest *tt = &gunzip_tests[i];
        const Error *want = gz_error(tt->err);
        Slice in = const_bytes(tt->gzip, tt->gzip_len);
        Slice raw = const_bytes(tt->raw, tt->raw_len);
        for (int plain = 0; plain < 2; plain++) {
            BytesReader br;
            bytes_reader_reset(&br, in);
            PlainReader pr = {bytes_reader_as_io_reader(&br)};
            IoReader src = plain ? (IoReader){&plain_reader_vt, &pr} : pr.r;
            Error err;
            GzipReader *r2 = gzip_new_reader(a, src, &err);
            if (r2 == NULL) {
                testing_t_errorf_v(t, "%s: NewReader: %s", tt->desc, error_text(err));
                continue;
            }
            for (int round = 0; round < 2; round++) {
                if (round == 1) {
                    bytes_reader_reset(&br, in);
                    err = gzip_reader_reset(r2, src);
                    if (BURROW_FAILED(err)) {
                        testing_t_errorf_v(t, "%s: Reset: %s", tt->desc,
                                           error_text(err));
                        break;
                    }
                }
                if (!str_is(r2->header.name, tt->name))
                    testing_t_errorf_v(t, "%s: got name %s", tt->desc, r2->header.name);
                BytesBuffer b = BYTES_BUFFER(a);
                int64_t n = io_copy(a, bytes_buffer_as_io_writer(&b),
                                    gzip_reader_as_io_reader(r2), &err);
                bool ok = want == NULL ? BURROW_OK(err) : same_error(err, *want);
                if (!ok)
                    testing_t_errorf_v(
                        t, "%s: io.Copy: %s want %s", tt->desc, error_text(err),
                        want != NULL ? error_text(*want) : BURROW_S("<nil>"));
                Slice s = bytes_buffer_bytes(&b);
                if (!bytes_eq(s, raw))
                    testing_t_errorf_v(t, "%s: got %d-byte %q want %d-byte %q",
                                       tt->desc, (int)n, str_from_bytes(s.p, s.len),
                                       tt->raw_len, str_from_bytes(raw.p, raw.len));
            }
            gzip_reader_free(r2);
        }
    }
    arena_free(&ar);
}

/* Go runs the copy on a goroutine and gives it a second. Here the copy just
 * has to come back, with an error. */
static void TestIssue6550(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice gz = gen_bytes(a, issue6550);
    BytesReader br;
    bytes_reader_reset(&br, gz);
    Error err;
    GzipReader *zr = gzip_new_reader(a, bytes_reader_as_io_reader(&br), &err);
    if (zr == NULL) {
        testing_t_fatalf_v(t, "NewReader(testdata/issue6550.gz): %s", error_text(err));
        arena_free(&ar);
        return;
    }
    (void)io_copy(a, io_discard, gzip_reader_as_io_reader(zr), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Copy succeeded");
    else
        testing_t_logf_v(t, "Copy failed (correctly): %s", error_text(err));
    gzip_reader_free(zr);
    arena_free(&ar);
}

static void TestMultistreamFalse(TestingT *t) {
    /* Find concatenation test. */
    const GunzipTest *tt = NULL;
    for (size_t i = 0; i < sizeof gunzip_tests / sizeof gunzip_tests[0]; i++) {
        const char *d = gunzip_tests[i].desc;
        size_t n = strlen(d);
        if (n >= 3 && strcmp(d + n - 3, " x2") == 0) {
            tt = &gunzip_tests[i];
            break;
        }
    }
    if (tt == NULL) {
        testing_t_fatalf_v(t, "cannot find hello.txt x2 in gunzip tests");
        return;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br, const_bytes(tt->gzip, tt->gzip_len));
    Error err;
    GzipReader *r = gzip_new_reader(a, bytes_reader_as_io_reader(&br), &err);
    if (r == NULL) {
        testing_t_fatalf_v(t, "r is NULL");
        return;
    }

    /* Expect two streams with "hello world\n", then real EOF. */
    Slice hello = BS("hello world\n");

    gzip_reader_multistream(r, false);
    Slice data = io_read_all(a, gzip_reader_as_io_reader(r), &err);
    if (!bytes_eq(data, hello) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "first stream = %q, %s, want %q, <nil>",
                           str_from_bytes(data.p, data.len), error_text(err),
                           BURROW_S("hello world\n"));

    err = gzip_reader_reset(r, bytes_reader_as_io_reader(&br));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "second reset: %s", error_text(err));
    gzip_reader_multistream(r, false);
    data = io_read_all(a, gzip_reader_as_io_reader(r), &err);
    if (!bytes_eq(data, hello) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "second stream = %q, %s, want %q, <nil>",
                           str_from_bytes(data.p, data.len), error_text(err),
                           BURROW_S("hello world\n"));

    err = gzip_reader_reset(r, bytes_reader_as_io_reader(&br));
    if (!same_error(err, io_eof))
        testing_t_fatalf_v(t, "third reset: err=%s, want io.EOF", error_text(err));
    gzip_reader_free(r);
    arena_free(&ar);
}

static void TestNilStream(TestingT *t) {
    /* Go liberally interprets RFC 1952 section 2.2 to mean that a gzip file
     * consist of zero or more members. Thus, we test that a nil stream is
     * okay. */
    BytesReader br;
    bytes_reader_reset(&br, no_bytes);
    Error err;
    GzipReader *zr =
        gzip_new_reader(heap_allocator(), bytes_reader_as_io_reader(&br), &err);
    if (zr != NULL || !same_error(err, io_eof))
        testing_t_fatalf_v(t, "NewReader(nil) on empty stream: got %s, want io.EOF",
                           error_text(err));
    gzip_reader_free(zr);
}

static void TestTruncatedStreams(TestingT *t) {
    static const char original[] =
        "\x1f\x8b\b\x04\x00\tn\x88\x00\xff\a\x00foo bar\xcbH\xcd\xc9\xc9\xd7Q(\xcf/"
        "\xcaI\x01\x04:r\xab\xff\f\x00\x00\x00";
    static const char truncated_name[] = "\x1f\x8b\x08\x10\x00\x00\x00\x00\x00\xff\x01";
    static const char truncated_comment[] =
        "\x1f\x8b\x08\x08\x00\x00\x00\x00\x00\xff\x01";
    static const struct {
        const char *name;
        const char *data;
        Int len;
    } cases[] = {
        {"original", original, (Int)sizeof original - 1},
        {"truncated name", truncated_name, (Int)sizeof truncated_name - 1},
        {"truncated comment", truncated_comment, (Int)sizeof truncated_comment - 1},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Intentionally iterate starting with at least one byte in the stream. */
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        for (Int i = 1; i < cases[c].len; i++) {
            BytesReader br;
            bytes_reader_reset(&br, const_bytes(cases[c].data, i));
            Error err;
            GzipReader *r = gzip_new_reader(a, bytes_reader_as_io_reader(&br), &err);
            if (r == NULL) {
                if (!same_error(err, io_err_unexpected_eof))
                    testing_t_errorf_v(t,
                                       "NewReader(%s-%d) on truncated stream: got %s, "
                                       "want unexpected EOF",
                                       cases[c].name, (int)i, error_text(err));
                continue;
            }
            (void)io_copy(a, io_discard, gzip_reader_as_io_reader(r), &err);
            if (!same_error(err, io_err_unexpected_eof))
                testing_t_errorf_v(t,
                                   "io.Copy(%s-%d) on truncated stream: got %s, want "
                                   "unexpected EOF",
                                   cases[c].name, (int)i, error_text(err));
            gzip_reader_free(r);
        }
    }
    arena_free(&ar);
}

/* Go builds four million copies of an empty member in memory. This reader
 * hands out the same bytes without keeping them. */
typedef struct RepeatReader {
    const Byte *p;
    Int len;
    Int left; /* copies still to give */
    Int off;
} RepeatReader;

static Int repeat_read(void *self, Slice p, Error *err) {
    RepeatReader *r = (RepeatReader *)self;
    Int n = 0;
    while (n < p.len && r->left > 0) {
        Int k = r->len - r->off;
        if (k > p.len - n)
            k = p.len - n;
        memcpy((Byte *)p.p + n, r->p + r->off, (size_t)k);
        n += k;
        r->off += k;
        if (r->off == r->len) {
            r->off = 0;
            r->left--;
        }
    }
    *err = n == 0 && p.len > 0 ? io_eof : BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT repeat_reader_vt = {NULL, repeat_read};

static void TestCVE202230631(TestingT *t) {
    static const Byte empty[] = {0x1f, 0x8b, 0x08, 0x00, 0xa7, 0x8f, 0x43,
                                 0x62, 0x00, 0x03, 0x03, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    RepeatReader rr = {empty, (Int)sizeof empty, 4000000, 0};
    Error err;
    GzipReader *z =
        gzip_new_reader(heap_allocator(), (IoReader){&repeat_reader_vt, &rr}, &err);
    if (z == NULL) {
        testing_t_fatalf_v(t, "NewReader: got %s, want nil", error_text(err));
        return;
    }
    /* Prior to CVE-2022-30631 fix, this would cause an unrecoverable panic due
     * to stack exhaustion. */
    Byte buf[10];
    (void)gzip_reader_read(z, slice_from(buf, 10, 10, TYPE_BYTE), &err);
    if (!same_error(err, io_eof))
        testing_t_errorf_v(t, "Reader.Read: got %s, want io.EOF", error_text(err));
    gzip_reader_free(z);
}

/* ------------------------------------------------------------- gzip_test.go */

static void TestEmpty(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);

    GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err = gzip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Writer.Close: %s", error_text(err));

    GzipReader *r = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
    if (r == NULL) {
        testing_t_fatalf_v(t, "NewReader: %s", error_text(err));
        return;
    }
    CHECK(r->header.comment.len == 0 && r->header.extra.p == NULL &&
          time_is_zero(r->header.mod_time) && r->header.name.len == 0 &&
          r->header.os == 255);
    Slice b = io_read_all(a, gzip_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAll: %s", error_text(err));
    if (b.len != 0)
        testing_t_fatalf_v(t, "got %d bytes, want 0", (int)b.len);
    err = gzip_reader_close(r);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Reader.Close: %s", error_text(err));
    gzip_reader_free(r);
    gzip_writer_free(w);
    arena_free(&ar);
}

static void TestRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);

    GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    w->header.comment = BURROW_S("comment");
    w->header.extra = BS("extra");
    w->header.mod_time = time_from_unix(100000000, 0);
    w->header.name = BURROW_S("name");
    Error err;
    gzip_writer_write(w, BS("payload"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write: %s", error_text(err));
    err = gzip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Writer.Close: %s", error_text(err));

    GzipReader *r = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
    if (r == NULL) {
        testing_t_fatalf_v(t, "NewReader: %s", error_text(err));
        return;
    }
    Slice b = io_read_all(a, gzip_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAll: %s", error_text(err));
    if (!bytes_eq(b, BS("payload")))
        testing_t_fatalf_v(t, "payload is %q, want %q", str_from_bytes(b.p, b.len),
                           BURROW_S("payload"));
    if (!str_is(r->header.comment, "comment"))
        testing_t_fatalf_v(t, "comment is %q, want %q", r->header.comment,
                           BURROW_S("comment"));
    if (!bytes_eq(r->header.extra, BS("extra")))
        testing_t_fatalf_v(t, "extra is %q, want %q",
                           str_from_bytes(r->header.extra.p, r->header.extra.len),
                           BURROW_S("extra"));
    if (time_unix(r->header.mod_time) != 100000000)
        testing_t_fatalf_v(t, "mtime is %d, want %d",
                           (int)time_unix(r->header.mod_time), 100000000);
    if (!str_is(r->header.name, "name"))
        testing_t_fatalf_v(t, "name is %q, want %q", r->header.name, BURROW_S("name"));
    err = gzip_reader_close(r);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Reader.Close: %s", error_text(err));
    gzip_reader_free(r);
    gzip_writer_free(w);
    arena_free(&ar);
}

/* Go calls readString and writeString directly. Here the name goes through a
 * whole header each way. */
static void TestLatin1(TestingT *t) {
    static const Byte latin1[] = {0xc4, 'u', 0xdf, 'e', 'r', 'u', 'n', 'g', 0};
    static const char utf8[] = "Äußerung";
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BytesBuffer in = BYTES_BUFFER(a);
    static const Byte head[] = {0x1f, 0x8b, 0x08, 0x08, 0, 0, 0, 0, 0, 0xff};
    static const Byte tail[] = {0x03, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
    Error err;
    bytes_buffer_write(&in, slice_from((void *)(uintptr_t)head, 10, 10, TYPE_BYTE),
                       &err);
    bytes_buffer_write(&in, slice_from((void *)(uintptr_t)latin1, 9, 9, TYPE_BYTE),
                       &err);
    bytes_buffer_write(&in, slice_from((void *)(uintptr_t)tail, 10, 10, TYPE_BYTE),
                       &err);
    GzipReader *z = gzip_new_reader(a, bytes_buffer_as_io_reader(&in), &err);
    if (z == NULL) {
        testing_t_fatalf_v(t, "readString: %s", error_text(err));
        return;
    }
    if (!str_is(z->header.name, utf8))
        testing_t_fatalf_v(t, "read latin-1: got %q, want %q", z->header.name,
                           BURROW_S("Äußerung"));
    gzip_reader_free(z);

    BytesBuffer buf = BYTES_BUFFER(a);
    GzipWriter *c = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    c->header.name = BURROW_S("Äußerung");
    err = gzip_writer_close(c);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "writeString: %s", error_text(err));
    Slice s = bytes_buffer_bytes(&buf);
    if (s.len < 19 || memcmp((Byte *)s.p + 10, latin1, 9) != 0)
        testing_t_fatalf_v(t, "write utf-8: got %q, want %q",
                           str_from_bytes((Byte *)s.p + 10, s.len - 10),
                           str_from_bytes(latin1, 9));
    gzip_writer_free(c);
    arena_free(&ar);
}

static void TestLatin1RoundTrip(TestingT *t) {
#define LATIN1_CASE(lit, ok)                                                           \
    {                                                                                  \
        lit, (Int)sizeof(lit) - 1, ok                                                  \
    }
    static const struct {
        const char *name;
        Int len;
        bool ok;
    } test_cases[] = {
        LATIN1_CASE("", true),
        LATIN1_CASE("ASCII is OK", true),
        LATIN1_CASE("unless it contains a NUL\x00", false),
        LATIN1_CASE("no matter where \x00 occurs", false),
        LATIN1_CASE("\x00\x00\x00", false),
        LATIN1_CASE("Látin-1 also passes (U+00E1)", true),
        LATIN1_CASE("but LĀtin Extended-A (U+0100) does not", false),
        LATIN1_CASE("neither does 日本語", false),
        LATIN1_CASE("invalid UTF-8 also \xff"
                    "ails",
                    false),
        LATIN1_CASE("\x00 as does Látin-1 with NUL", false),
    };
#undef LATIN1_CASE
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof test_cases / sizeof test_cases[0]; i++) {
        Str name = {(const Byte *)test_cases[i].name, test_cases[i].len};
        BytesBuffer buf = BYTES_BUFFER(a);

        GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
        w->header.name = name;
        Error err = gzip_writer_close(w);
        gzip_writer_free(w);
        if (BURROW_OK(err) != test_cases[i].ok) {
            testing_t_errorf_v(t, "Writer.Close: name = %q, err = %s", name,
                               error_text(err));
            continue;
        }
        if (!test_cases[i].ok)
            continue;

        GzipReader *r = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
        if (r == NULL) {
            testing_t_errorf_v(t, "NewReader: %s", error_text(err));
            continue;
        }
        (void)io_read_all(a, gzip_reader_as_io_reader(r), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "ReadAll: %s", error_text(err));
        } else if (!str_eq(r->header.name, name)) {
            testing_t_errorf_v(t, "name is %q, want %q", r->header.name, name);
        } else {
            err = gzip_reader_close(r);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "Reader.Close: %s", error_text(err));
        }
        gzip_reader_free(r);
    }
    arena_free(&ar);
}

static void TestWriterFlush(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);

    GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    w->header.comment = BURROW_S("comment");
    w->header.extra = BS("extra");
    w->header.mod_time = time_from_unix(100000000, 0);
    w->header.name = BURROW_S("name");

    Int n0 = bytes_buffer_len(&buf);
    if (n0 != 0)
        testing_t_fatalf_v(t, "buffer size = %d before writes; want 0", (int)n0);

    Error err = gzip_writer_flush(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    Int n1 = bytes_buffer_len(&buf);
    if (n1 == 0)
        testing_t_fatalf_v(t, "no data after first flush");

    gzip_writer_write(w, BS("x"), &err);

    Int n2 = bytes_buffer_len(&buf);
    if (n1 != n2)
        testing_t_fatalf_v(t,
                           "after writing a single byte, size changed from %d to %d; "
                           "want no change",
                           (int)n1, (int)n2);

    err = gzip_writer_flush(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    Int n3 = bytes_buffer_len(&buf);
    if (n2 == n3)
        testing_t_fatalf_v(t, "Flush didn't flush any data");

    err = gzip_writer_close(w);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    gzip_writer_free(w);
    arena_free(&ar);
}

static void TestConcat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err;
    GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    gzip_writer_write(w, BS("hello "), &err);
    (void)gzip_writer_close(w);
    gzip_writer_free(w);
    w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    gzip_writer_write(w, BS("world\n"), &err);
    (void)gzip_writer_close(w);
    gzip_writer_free(w);

    GzipReader *r = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
    if (r == NULL) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return;
    }
    Slice data = io_read_all(a, gzip_reader_as_io_reader(r), &err);
    if (!bytes_eq(data, BS("hello world\n")) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAll = %q, %s, want %q, nil",
                           str_from_bytes(data.p, data.len), error_text(err),
                           BURROW_S("hello world"));
    gzip_reader_free(r);
    arena_free(&ar);
}

static void TestWriterReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    BytesBuffer buf2 = BYTES_BUFFER(a);
    GzipWriter *z = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Slice msg = BS("hello world");
    Error err;
    gzip_writer_write(z, msg, &err);
    (void)gzip_writer_close(z);
    gzip_writer_reset(z, bytes_buffer_as_io_writer(&buf2));
    gzip_writer_write(z, msg, &err);
    (void)gzip_writer_close(z);
    if (!bytes_eq(bytes_buffer_bytes(&buf), bytes_buffer_bytes(&buf2)))
        testing_t_errorf_v(t, "buf2 %q != original buf of %q",
                           bytes_buffer_string(&buf2, a), bytes_buffer_string(&buf, a));
    gzip_writer_free(z);
    arena_free(&ar);
}

typedef struct LimitedWriter {
    Int n;
} LimitedWriter;

static Int limited_write(void *self, Slice p, Error *err) {
    LimitedWriter *l = (LimitedWriter *)self;
    Int n = l->n;
    if (n < p.len) {
        l->n = 0;
        *err = io_err_short_write;
        return n;
    }
    l->n -= p.len;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT limited_writer_vt = {NULL, limited_write};

static void TestLimitedWrite(TestingT *t) {
    Slice msg = BS("a");

    for (Int lim = 2; lim < 20; lim++) {
        LimitedWriter l1 = {lim};
        GzipWriter *z =
            gzip_new_writer(heap_allocator(), (IoWriter){&limited_writer_vt, &l1});
        Error err;
        Int n = gzip_writer_write(z, msg, &err);
        if (n > msg.len)
            testing_t_errorf_v(t, "Write() = %d, want %d or less", (int)n,
                               (int)msg.len);

        LimitedWriter l2 = {lim};
        gzip_writer_reset(z, (IoWriter){&limited_writer_vt, &l2});
        z->header = (GzipHeader){
            .comment = BURROW_S("comment"),
            .extra = BS("extra"),
            .mod_time = time_now(),
            .name = BURROW_S("name"),
            .os = 1,
        };
        n = gzip_writer_write(z, msg, &err);
        if (n > msg.len)
            testing_t_errorf_v(t, "Write() = %d, want %d or less", (int)n,
                               (int)msg.len);
        gzip_writer_free(z);
    }
}

/* ---------------------------------------------------------- example_test.go */

static void TestExampleWriterReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    GzipWriter *zw = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));

    /* Setting the Header fields is optional. */
    zw->header.name = BURROW_S("a-new-hope.txt");
    zw->header.comment = BURROW_S("an epic space opera by George Lucas");
    zw->header.mod_time = time_date(1977, TIME_MAY, 25, 0, 0, 0, 0, time_utc_loc);

    Error err;
    gzip_writer_write(zw, BS("A long time ago in a galaxy far, far away..."), &err);
    CHECK(BURROW_OK(err));
    CHECK(BURROW_OK(gzip_writer_close(zw)));

    GzipReader *zr = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
    if (zr == NULL) {
        testing_t_fatalf_v(t, "zr is NULL");
        return;
    }
    CHECK(str_is(zr->header.name, "a-new-hope.txt"));
    CHECK(str_is(zr->header.comment, "an epic space opera by George Lucas"));
    CHECK(str_is(time_string(time_utc(zr->header.mod_time), a),
                 "1977-05-25 00:00:00 +0000 UTC"));
    Slice got = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
    CHECK(BURROW_OK(err));
    CHECK(bytes_eq(got, BS("A long time ago in a galaxy far, far away...")));
    CHECK(BURROW_OK(gzip_reader_close(zr)));
    gzip_reader_free(zr);
    gzip_writer_free(zw);
    arena_free(&ar);
}

static void TestExampleReaderMultistream(TestingT *t) {
    static const struct {
        const char *name;
        const char *comment;
        int year, month, day, hour, min, sec, nsec;
        const char *data;
    } files[] = {
        {"file-1.txt", "file-header-1", 2006, 2, 1, 3, 4, 5, 0, "Hello Gophers - 1"},
        {"file-2.txt", "file-header-2", 2007, 3, 2, 4, 5, 6, 1, "Hello Gophers - 2"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    GzipWriter *zw = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err;
    for (int i = 0; i < 2; i++) {
        zw->header.name = str_from_cstr(files[i].name);
        zw->header.comment = str_from_cstr(files[i].comment);
        zw->header.mod_time = time_date(files[i].year, (TimeMonth)files[i].month,
                                        files[i].day, files[i].hour, files[i].min,
                                        files[i].sec, files[i].nsec, time_utc_loc);
        gzip_writer_write(zw, const_bytes(files[i].data, (Int)strlen(files[i].data)),
                          &err);
        CHECK(BURROW_OK(err));
        CHECK(BURROW_OK(gzip_writer_close(zw)));
        gzip_writer_reset(zw, bytes_buffer_as_io_writer(&buf));
    }

    GzipReader *zr = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
    if (zr == NULL) {
        testing_t_fatalf_v(t, "zr is NULL");
        return;
    }
    BytesBuffer out = BYTES_BUFFER(a);
    for (;;) {
        gzip_reader_multistream(zr, false);
        fmt_fprintf_v(bytes_buffer_as_io_writer(&out),
                      "Name: %s\nComment: %s\nModTime: %s\n\n", zr->header.name,
                      zr->header.comment,
                      time_string(time_utc(zr->header.mod_time), a));
        (void)io_copy(a, bytes_buffer_as_io_writer(&out), gzip_reader_as_io_reader(zr),
                      &err);
        CHECK(BURROW_OK(err));
        bytes_buffer_write_string(&out, BURROW_S("\n\n"), &err);
        err = gzip_reader_reset(zr, bytes_buffer_as_io_reader(&buf));
        if (same_error(err, io_eof))
            break;
        CHECK(BURROW_OK(err));
    }
    CHECK(BURROW_OK(gzip_reader_close(zr)));
    CHECK(str_is(bytes_buffer_string(&out, a),
                 "Name: file-1.txt\n"
                 "Comment: file-header-1\n"
                 "ModTime: 2006-02-01 03:04:05 +0000 UTC\n"
                 "\n"
                 "Hello Gophers - 1\n"
                 "\n"
                 "Name: file-2.txt\n"
                 "Comment: file-header-2\n"
                 "ModTime: 2007-03-02 04:05:06 +0000 UTC\n"
                 "\n"
                 "Hello Gophers - 2\n"
                 "\n"));
    gzip_reader_free(zr);
    gzip_writer_free(zw);
    arena_free(&ar);
}

/* ------------------------------------------------------------ burrow's own */

/* What Go's writer makes, byte for byte, at every level with and without a
 * full header, and each of those reads back. */
static void TestGoVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    int last = -1;
    Slice in = no_bytes;
    for (size_t i = 0; i < sizeof gzip_vectors / sizeof gzip_vectors[0]; i++) {
        const GzipVector *v = &gzip_vectors[i];
        if (v->input != last) {
            in = gzip_input(a, v->input);
            last = v->input;
        }
        Slice got = gzip_all(a, v->level, v->header, in);
        char hex[65];
        sha_hex(got, hex);
        if (got.len != v->len || strcmp(hex, v->sha256) != 0)
            testing_t_errorf_v(
                t, "input %d, level %d, header %d: got %d bytes, Go made %d", v->input,
                v->level, v->header, (int)got.len, (int)v->len);
        Error err;
        Slice back = gunzip_all(a, got, v->input % 2 == 0, &err);
        if (BURROW_FAILED(err) || !bytes_eq(back, in))
            testing_t_errorf_v(t,
                               "input %d, level %d, header %d: does not read back: %s",
                               v->input, v->level, v->header, error_text(err));
    }
    arena_free(&ar);
}

/* The full header reads back field for field, including an extra with a NUL
 * in it and a comment that went out as Latin-1. */
static void TestHeaderRoundTrip(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice z = gzip_all(a, 6, 1, BS("x"));
    BytesReader br;
    bytes_reader_reset(&br, z);
    Error err;
    GzipReader *r = gzip_new_reader(a, bytes_reader_as_io_reader(&br), &err);
    if (r == NULL) {
        testing_t_fatalf_v(t, "r is NULL");
        return;
    }
    CHECK(str_is(r->header.comment, "Grüße aus Gettysburg"));
    CHECK(bytes_eq(r->header.extra, BS("ex\0tra")));
    CHECK(time_unix(r->header.mod_time) == 1000000000);
    CHECK(str_is(r->header.name, "gettysburg.txt"));
    CHECK(r->header.os == 3);
    gzip_reader_free(r);
    arena_free(&ar);
}

/* An extra field of no bytes is there but empty, and a nil one is not there
 * at all. */
static void TestEmptyExtra(TestingT *t) {
    (void)t;
    static const Byte nothing = 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int with = 0; with < 2; with++) {
        BytesBuffer buf = BYTES_BUFFER(a);
        GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
        if (with)
            w->header.extra = slice_from((void *)(uintptr_t)&nothing, 0, 0, TYPE_BYTE);
        CHECK(BURROW_OK(gzip_writer_close(w)));
        gzip_writer_free(w);
        Slice s = bytes_buffer_bytes(&buf);
        CHECK(((Byte *)s.p)[3] == (with ? 0x04 : 0));
        Error err;
        GzipReader *r = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
        if (r == NULL) {
            testing_t_fatalf_v(t, "r is NULL");
            return;
        }
        CHECK((r->header.extra.p != NULL) == (with != 0));
        CHECK(r->header.extra.len == 0);
        gzip_reader_free(r);
    }
    arena_free(&ar);
}

/* FHCRC: a header with the low 16 bits of its CRC-32 after it, which the
 * reader checks. */
static void TestHeaderCRC(TestingT *t) {
    (void)t;
    Byte s[22] = {0x1f, 0x8b, 0x08, 0x02, 0, 0, 0, 0, 0, 0xff};
    uint32_t crc = crc32_checksum_ieee(slice_from(s, 10, 10, TYPE_BYTE));
    s[12] = 0x03;
    for (int bad = 0; bad < 2; bad++) {
        s[10] = (Byte)(crc ^ (uint32_t)bad);
        s[11] = (Byte)(crc >> 8);
        Error err;
        Slice got =
            gunzip_all(heap_allocator(), slice_from(s, 22, 22, TYPE_BYTE), false, &err);
        if (bad)
            CHECK(same_error(err, gzip_err_header));
        else
            CHECK(BURROW_OK(err) && got.len == 0);
        mem_free(heap_allocator(), got.p, (size_t)got.cap, 1);
    }
}

/* A name longer than the 512 bytes Go reads a header string into is a bad
 * header. */
static void TestLongName(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int n = 510; n <= 512; n++) {
        Byte *name = (Byte *)mem_alloc(a, (size_t)n, 1);
        memset(name, 'n', (size_t)n);
        BytesBuffer buf = BYTES_BUFFER(a);
        GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
        w->header.name = (Str){name, n};
        CHECK(BURROW_OK(gzip_writer_close(w)));
        gzip_writer_free(w);
        Error err;
        GzipReader *r = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
        if (n < 512) {
            CHECK(r != NULL && r->header.name.len == n);
        } else {
            CHECK(r == NULL && same_error(err, gzip_err_header));
        }
        gzip_reader_free(r);
    }
    arena_free(&ar);
}

static void TestExtraTooLarge(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    GzipWriter *w = gzip_new_writer(a, io_discard);
    w->header.extra = make_bytes(a, 0x10000);
    Error err = gzip_writer_close(w);
    CHECK(str_is(error_text(err), "gzip.Write: Extra data is too large"));
    gzip_writer_free(w);
    arena_free(&ar);
}

static void TestInvalidLevel(TestingT *t) {
    static const Int levels[] = {-3, 10, 100, -2147483647 - 1};
    static const char *const want[] = {
        "gzip: invalid compression level: -3",
        "gzip: invalid compression level: 10",
        "gzip: invalid compression level: 100",
        "gzip: invalid compression level: -2147483648",
    };
    for (int i = 0; i < 4; i++) {
        Error err;
        GzipWriter *w =
            gzip_new_writer_level(heap_allocator(), io_discard, levels[i], &err);
        CHECK(w == NULL);
        CHECK(str_is(error_text(err), want[i]));
    }
}

/* Close twice is fine, and Flush and Write after Close. */
static void TestWriterAfterClose(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    CHECK(BURROW_OK(gzip_writer_close(w)));
    Int n = bytes_buffer_len(&buf);
    CHECK(BURROW_OK(gzip_writer_close(w)));
    CHECK(BURROW_OK(gzip_writer_flush(w)));
    CHECK(bytes_buffer_len(&buf) == n);
    gzip_writer_free(w);
    arena_free(&ar);
}

/* The reader and writer as io interfaces, and freeing NULL. */
static void TestInterfaces(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    GzipWriter *w = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    IoWriteCloser wc = gzip_writer_as_io_write_closer(w);
    IoWriter iw = io_write_closer_as_io_writer(wc);
    Error err;
    CHECK_INT_EQ((int)iw.vt->write(iw.data, BS("hello, world\n"), &err), 13);
    CHECK(BURROW_OK(wc.vt->closer.close(wc.data)));
    CHECK(gzip_writer_as_io_writer(w).data == w);
    CHECK(wc.vt->writer.self_type == TYPE_GZIP_WRITER);

    GzipReader *r = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
    IoReadCloser rc = gzip_reader_as_io_read_closer(r);
    CHECK(rc.vt->reader.self_type == TYPE_GZIP_READER);
    Slice got = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    CHECK(BURROW_OK(err) && bytes_eq(got, BS("hello, world\n")));
    CHECK(BURROW_OK(rc.vt->closer.close(rc.data)));
    gzip_reader_free(r);
    gzip_writer_free(w);
    gzip_reader_free(NULL);
    gzip_writer_free(NULL);
    arena_free(&ar);
}

/* An allocator that fails after a budget of allocations and counts what is
 * live, to check that running out of memory anywhere leaks nothing. */
typedef struct Budget {
    int left;
    long long live;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *p = mem_alloc(heap_allocator(), size, align);
    if (p != NULL)
        b->live += (long long)size;
    return p;
}

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *q = mem_realloc(heap_allocator(), p, old, nsz, align);
    if (q != NULL)
        b->live += (long long)nsz - (long long)old;
    return q;
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (p == NULL)
        return;
    b->live -= (long long)size;
    mem_free(heap_allocator(), p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, budget_realloc,
                                  budget_free,  NULL, NULL};

static void TestNoMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *scratch = arena_allocator(&ar);
    Slice in = gzip_input(scratch, 0);
    Slice z = gzip_all(scratch, 6, 1, in);

    /* The reader, from a source without ReadByte so that it needs a buffer,
     * and with a header whose strings it has to keep. */
    bool done = false;
    for (int budget = 0; budget < 64 && !done; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesReader br;
        bytes_reader_reset(&br, z);
        PlainReader pr = {bytes_reader_as_io_reader(&br)};
        Error err;
        GzipReader *r = gzip_new_reader(&al, (IoReader){&plain_reader_vt, &pr}, &err);
        if (r == NULL) {
            if (!same_error(err, burrow_err_out_of_memory))
                testing_t_errorf_v(t, "reader, budget %d: %s", budget, error_text(err));
        } else {
            Slice got = io_read_all(scratch, gzip_reader_as_io_reader(r), &err);
            if (BURROW_OK(err)) {
                done = true;
                CHECK(bytes_eq(got, in));
            } else if (!same_error(err, burrow_err_out_of_memory)) {
                testing_t_errorf_v(t, "reader, budget %d: read: %s", budget,
                                   error_text(err));
            }
            gzip_reader_free(r);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "reader, budget %d: %d bytes leaked", budget,
                               (int)b.live);
    }
    CHECK(done);

    /* The writer, which allocates the compressor with the header and a
     * buffer for a Latin-1 string. */
    done = false;
    for (int budget = 0; budget < 64 && !done; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesBuffer out = BYTES_BUFFER(scratch);
        Error err;
        GzipWriter *w =
            gzip_new_writer_level(&al, bytes_buffer_as_io_writer(&out), 6, &err);
        if (w == NULL) {
            if (!same_error(err, burrow_err_out_of_memory))
                testing_t_errorf_v(t, "writer, budget %d: %s", budget, error_text(err));
        } else {
            gzip_set_header(w, 1);
            gzip_writer_write(w, in, &err);
            Error cerr = gzip_writer_close(w);
            if (BURROW_OK(err) && BURROW_OK(cerr)) {
                done = true;
                CHECK(bytes_eq(bytes_buffer_bytes(&out), z));
            } else if (!same_error(err, burrow_err_out_of_memory) ||
                       !same_error(cerr, burrow_err_out_of_memory)) {
                testing_t_errorf_v(t, "writer, budget %d: %s, %s", budget,
                                   error_text(err), error_text(cerr));
            }
            gzip_writer_free(w);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "writer, budget %d: %d bytes leaked", budget,
                               (int)b.live);
    }
    CHECK(done);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDecompressor)                                                                \
    X(TestIssue6550)                                                                   \
    X(TestMultistreamFalse)                                                            \
    X(TestNilStream)                                                                   \
    X(TestTruncatedStreams)                                                            \
    X(TestCVE202230631)                                                                \
    X(TestEmpty)                                                                       \
    X(TestRoundTrip)                                                                   \
    X(TestLatin1)                                                                      \
    X(TestLatin1RoundTrip)                                                             \
    X(TestWriterFlush)                                                                 \
    X(TestConcat)                                                                      \
    X(TestWriterReset)                                                                 \
    X(TestLimitedWrite)                                                                \
    X(TestExampleWriterReader)                                                         \
    X(TestExampleReaderMultistream)                                                    \
    X(TestGoVectors)                                                                   \
    X(TestHeaderRoundTrip)                                                             \
    X(TestEmptyExtra)                                                                  \
    X(TestHeaderCRC)                                                                   \
    X(TestLongName)                                                                    \
    X(TestExtraTooLarge)                                                               \
    X(TestInvalidLevel)                                                                \
    X(TestWriterAfterClose)                                                            \
    X(TestInterfaces)                                                                  \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
