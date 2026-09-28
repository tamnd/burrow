/* Derived from Go's src/compress/zlib/reader_test.go, writer_test.go and
 * example_test.go.
 * Go source: go1.27.1.
 *
 * Go's writer tests read e.txt and pi.txt from testdata. The words and noise
 * compress/flate's tests make stand in for them here, and tests/zlib_test_gen.h,
 * from tools/gen-zlib-tests.sh, has the SHA-256 of what Go's writer makes of
 * them at every level, so the output is checked byte for byte as well as by
 * reading it back. Go pushes the data through an io.Pipe with the writer in a
 * goroutine; here the writer fills a buffer first and a reader with only Read
 * takes it back, which gives the reader the same bufio.Reader of its own.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/compress/zlib.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/base64.h"
#include "burrow/hash/adler32.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "flate_test_gen.h"
#include "zlib_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Slice no_dict = {NULL, 0, 0, NULL};

#define BS(lit)                                                                        \
    slice_from((void *)(uintptr_t)(lit), (Int)sizeof(lit) - 1, (Int)sizeof(lit) - 1,   \
               TYPE_BYTE)

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool text_is(Str s, const char *want) {
    return (size_t)s.len == strlen(want) && memcmp(s.p, want, (size_t)s.len) == 0;
}

static bool bytes_eq(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static Slice make_bytes(Alloc *a, Int n) {
    return slice_from(mem_alloc(a, (size_t)n + 1, 1), n, n, TYPE_BYTE);
}

static Slice bytes_of(Alloc *a, const Byte *p, Int n) {
    Slice b = make_bytes(a, n);
    if (n > 0)
        memcpy(b.p, p, (size_t)n);
    return b;
}

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

/* The words tools/gen-zlib-tests.sh compresses, made the same way. */
static Slice zlib_words(Alloc *a, Int n) {
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
static Slice zlib_noise(Alloc *a, Int n, uint64_t seed) {
    Slice b = make_bytes(a, n);
    uint64_t x = seed;
    for (Int i = 0; i < n; i++) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        ((Byte *)b.p)[i] = (Byte)(x >> 56);
    }
    return b;
}

/* The inputs the generator numbers. */
static Slice zlib_input(Alloc *a, int id) {
    switch (id) {
    case 0:
        return gen_bytes(a, gettysburg);
    case 1:
        return zlib_words(a, 34000);
    case 2:
        return zlib_noise(a, 70000, 7);
    case 3:
        return no_dict;
    default:
        return bytes_of(a, (const Byte *)"hello, world\n", 13);
    }
}

static const Byte empty_dict_byte = 0;

/* The dictionaries the generator numbers: none, empty and not nil, and
 * TestWriterDict's. */
static Slice zlib_dict_of(int id) {
    switch (id) {
    case 0:
        return no_dict;
    case 1:
        return slice_from((void *)(uintptr_t)&empty_dict_byte, 0, 0, TYPE_BYTE);
    default:
        return BS("0123456789.");
    }
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

/* A reader with only Read, Go's struct{ io.Reader }. */
typedef struct PlainReader {
    IoReader r;
} PlainReader;

static Int plain_read(void *self, Slice p, Error *err) {
    PlainReader *pr = (PlainReader *)self;
    return pr->r.vt->read(pr->r.data, p, err);
}

static const IoReaderVT plain_reader_vt = {NULL, plain_read};

/* A writer that fails with "IO error" on call n, counting from 0, and every
 * call after it. */
typedef struct FailWriter {
    int n;
} FailWriter;

static const Str err_io_text = {(const Byte *)"IO error", 8};
static const Error err_io = {&burrow_sentinel_error_vt, &err_io_text};

static Int fail_writer_write(void *self, Slice p, Error *err) {
    FailWriter *f = (FailWriter *)self;
    if (f->n <= 0) {
        *err = err_io;
        return 0;
    }
    f->n--;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT fail_writer_vt = {NULL, fail_writer_write};

/* Compresses in at level with dict, in one Write and a Close. */
static Slice deflate_all(Alloc *a, Int level, Slice dict, Slice in) {
    BytesBuffer out = BYTES_BUFFER(a);
    Error err;
    ZlibWriter *w = zlib_new_writer_level_dict(a, bytes_buffer_as_io_writer(&out),
                                               level, dict, &err);
    if (w == NULL) {
        fprintf(stderr, "zlib_new_writer_level_dict: %.*s\n", (int)error_text(err).len,
                (const char *)error_text(err).p);
        abort();
    }
    Int n = zlib_writer_write(w, in, &err);
    if (n != in.len || BURROW_FAILED(err))
        abort();
    if (BURROW_FAILED(zlib_writer_close(w)))
        abort();
    zlib_writer_free(w);
    return bytes_buffer_bytes(&out);
}

/* io.ReadAll of a zlib reader over in, through a reader with only Read when
 * plain is set. */
static Slice inflate_all(Alloc *a, Slice in, Slice dict, bool plain, Error *err) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    PlainReader pr = {bytes_reader_as_io_reader(&br)};
    IoReader src = plain ? (IoReader){&plain_reader_vt, &pr} : pr.r;
    IoReadCloser rc = zlib_new_reader_dict(a, src, dict, err);
    if (rc.vt == NULL)
        return no_dict;
    Slice out = io_read_all(a, io_read_closer_as_io_reader(rc), err);
    Error cerr = rc.vt->closer.close(rc.data);
    if (BURROW_OK(*err))
        *err = cerr;
    zlib_reader_free(rc);
    return out;
}

/* ------------------------------------------------------------ reader_test.go */

typedef struct ZlibTest {
    const char *desc;
    const char *raw;
    const Byte *compressed;
    Int compressed_len;
    const Byte *dict;
    Int dict_len;
    const Error *err;
} ZlibTest;

static const Byte zt_dict_byte[] = {0x00};
static const Byte zt_truncated_dict[] = {0x78, 0xbb};
static const Byte zt_truncated_checksum[] = {
    0x78, 0xbb, 0x00, 0x01, 0x00, 0x01, 0xca, 0x48, 0xcd, 0xc9, 0xc9, 0xd7,
    0x51, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01, 0x04, 0x00, 0x00, 0xff, 0xff,
};
static const Byte zt_empty[] = {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
static const Byte zt_goodbye[] = {
    0x78, 0x9c, 0x4b, 0xcf, 0xcf, 0x4f, 0x49, 0xaa, 0x4c, 0xd5, 0x51,
    0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01, 0x00, 0x28, 0xa5, 0x05, 0x5e,
};
static const Byte zt_bad_cinfo[] = {0x88, 0x98, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
static const Byte zt_bad_fcheck[] = {0x78, 0x9f, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
static const Byte zt_bad_checksum[] = {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0xff};
static const Byte zt_not_enough[] = {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00};
static const Byte zt_excess[] = {
    0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x78, 0x9c, 0xff,
};
static const Byte zt_dictionary[] = {
    0x78, 0xbb, 0x1c, 0x32, 0x04, 0x27, 0xf3, 0x00, 0xb1, 0x75,
    0x20, 0x1c, 0x45, 0x2e, 0x00, 0x24, 0x12, 0x04, 0x74,
};
static const Byte zt_hello_world_dict[] = {
    0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64, 0x0a,
};
static const Byte zt_raw_block[] = {
    0x78, 0x9c, 0x00, 0x0c, 0x00, 0xf3, 0xff, 0x68, 0x65, 0x6c, 0x6c, 0x6f,
};
static const Byte zt_fixed_block[] = {0x78, 0x9c, 0xf2, 0x48, 0xcd};

#define ZT_BYTES(b) b, (Int)sizeof b

static const ZlibTest zlib_tests[] = {
    {"truncated empty", "", NULL, 0, NULL, 0, &io_err_unexpected_eof},
    {"truncated dict", "", ZT_BYTES(zt_truncated_dict), ZT_BYTES(zt_dict_byte),
     &io_err_unexpected_eof},
    {"truncated checksum", "", ZT_BYTES(zt_truncated_checksum), ZT_BYTES(zt_dict_byte),
     &io_err_unexpected_eof},
    {"empty", "", ZT_BYTES(zt_empty), NULL, 0, NULL},
    {"goodbye", "goodbye, world", ZT_BYTES(zt_goodbye), NULL, 0, NULL},
    {"bad header (CINFO)", "", ZT_BYTES(zt_bad_cinfo), NULL, 0, &zlib_err_header},
    {"bad header (FCHECK)", "", ZT_BYTES(zt_bad_fcheck), NULL, 0, &zlib_err_header},
    {"bad checksum", "", ZT_BYTES(zt_bad_checksum), NULL, 0, &zlib_err_checksum},
    {"not enough data", "", ZT_BYTES(zt_not_enough), NULL, 0, &io_err_unexpected_eof},
    {"excess data is silently ignored", "", ZT_BYTES(zt_excess), NULL, 0, NULL},
    {"dictionary", "Hello, World!\n", ZT_BYTES(zt_dictionary),
     ZT_BYTES(zt_hello_world_dict), NULL},
    {"wrong dictionary", "", ZT_BYTES(zt_dictionary), zt_hello_world_dict, 4,
     &zlib_err_dictionary},
    {"truncated zlib stream amid raw-block", "hello", ZT_BYTES(zt_raw_block), NULL, 0,
     &io_err_unexpected_eof},
    {"truncated zlib stream amid fixed-block", "He", ZT_BYTES(zt_fixed_block), NULL, 0,
     &io_err_unexpected_eof},
};

/* Go's test lets a case that expects an error pass when the error does not
 * come at all. This one wants the error, from zlib_new_reader_dict or from
 * reading, and it runs every case through a reader with ReadByte and one
 * without. */
static void TestDecompressor(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof zlib_tests / sizeof zlib_tests[0]; i++) {
        const ZlibTest *tt = &zlib_tests[i];
        for (int plain = 0; plain < 2; plain++) {
            BytesReader br;
            bytes_reader_reset(&br, slice_from((void *)(uintptr_t)tt->compressed,
                                               tt->compressed_len, tt->compressed_len,
                                               TYPE_BYTE));
            PlainReader pr = {bytes_reader_as_io_reader(&br)};
            IoReader src = plain ? (IoReader){&plain_reader_vt, &pr} : pr.r;
            Slice dict = tt->dict != NULL
                             ? slice_from((void *)(uintptr_t)tt->dict, tt->dict_len,
                                          tt->dict_len, TYPE_BYTE)
                             : no_dict;
            Error err;
            IoReadCloser zr = zlib_new_reader_dict(a, src, dict, &err);
            if (zr.vt == NULL) {
                if (tt->err == NULL || !same_error(err, *tt->err))
                    testing_t_errorf_v(t, "%s: NewReader: %s", tt->desc,
                                       error_text(err));
                continue;
            }

            /* Read and verify correctness of data. */
            BytesBuffer b = BYTES_BUFFER(a);
            int64_t n = io_copy(a, bytes_buffer_as_io_writer(&b),
                                io_read_closer_as_io_reader(zr), &err);
            if (BURROW_FAILED(err) || tt->err != NULL) {
                if (tt->err == NULL || !same_error(err, *tt->err))
                    testing_t_errorf_v(
                        t, "%s: io.Copy: %s want %s", tt->desc, error_text(err),
                        tt->err != NULL ? error_text(*tt->err) : BURROW_S("<nil>"));
                /* The error is sticky, and Close gives it too. */
                Byte one[1];
                Int n1 =
                    zr.vt->reader.read(zr.data, slice_from(one, 1, 1, TYPE_BYTE), &err);
                if (n1 != 0 || tt->err == NULL || !same_error(err, *tt->err))
                    testing_t_errorf_v(t, "%s: sticky error lost", tt->desc);
                if (tt->err == NULL ||
                    !same_error(zr.vt->closer.close(zr.data), *tt->err))
                    testing_t_errorf_v(t, "%s: Close lost the error", tt->desc);
                zlib_reader_free(zr);
                continue;
            }
            Slice s = bytes_buffer_bytes(&b);
            if (!bytes_eq(s,
                          slice_from((void *)(uintptr_t)tt->raw, (Int)strlen(tt->raw),
                                     (Int)strlen(tt->raw), TYPE_BYTE)))
                testing_t_errorf_v(t, "%s: got %d-byte %q want %d-byte %q", tt->desc,
                                   (int)n, str_from_bytes(s.p, s.len),
                                   (int)strlen(tt->raw), tt->raw);

            /* Check for sticky errors. */
            Byte one[1];
            Int n1 =
                zr.vt->reader.read(zr.data, slice_from(one, 1, 1, TYPE_BYTE), &err);
            if (n1 != 0 || !same_error(err, io_eof))
                testing_t_errorf_v(t, "%s: Read() = (%d, %s), want (0, io.EOF)",
                                   tt->desc, (int)n1, error_text(err));
            Error cerr = zr.vt->closer.close(zr.data);
            if (BURROW_FAILED(cerr))
                testing_t_errorf_v(t, "%s: Close() = %s, want nil", tt->desc,
                                   error_text(cerr));
            zlib_reader_free(zr);
        }
    }
    arena_free(&ar);
}

/* With a ReadByte source the reader stops at the end of the checksum, so what
 * follows a stream is still there for whoever reads next. */
static void TestReaderStopsAtChecksum(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br,
                       slice_from((void *)(uintptr_t)zt_excess, (Int)sizeof zt_excess,
                                  (Int)sizeof zt_excess, TYPE_BYTE));
    Error err;
    IoReadCloser zr = zlib_new_reader(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(zr.vt != NULL);
    Slice got = io_read_all(a, io_read_closer_as_io_reader(zr), &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ((int)got.len, 0);
    CHECK_INT_EQ((int)bytes_reader_len(&br), 3);
    zlib_reader_free(zr);
    arena_free(&ar);
}

/* zlib.Resetter: one reader goes through several streams, with and without a
 * dictionary, from sources with and without ReadByte. */
static void TestReaderReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice words = zlib_input(a, 1);
    Slice dict = zlib_dict_of(2);
    Slice plain_stream = deflate_all(a, 6, no_dict, words);
    Slice dict_stream = deflate_all(a, 6, dict, words);

    BytesReader br;
    bytes_reader_reset(&br, plain_stream);
    Error err;
    IoReadCloser zr = zlib_new_reader(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(zr.vt != NULL);
    ZlibResetter rs = zlib_reader_as_resetter(zr);
    CHECK(rs.vt != NULL && rs.data == zr.data);
    for (int round = 0; round < 6; round++) {
        bool use_dict = round % 2 == 1;
        bool plain = round >= 3;
        if (round > 0) {
            bytes_reader_reset(&br, use_dict ? dict_stream : plain_stream);
            PlainReader pr = {bytes_reader_as_io_reader(&br)};
            IoReader src = plain ? (IoReader){&plain_reader_vt, &pr} : pr.r;
            err = zlib_resetter_reset(rs, src, use_dict ? dict : no_dict);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "round %d: Reset: %s", round, error_text(err));
                continue;
            }
            Slice got = io_read_all(a, io_read_closer_as_io_reader(zr), &err);
            if (BURROW_FAILED(err) || !bytes_eq(got, words))
                testing_t_errorf_v(t, "round %d: got %d bytes, %s", round, (int)got.len,
                                   error_text(err));
        } else {
            Slice got = io_read_all(a, io_read_closer_as_io_reader(zr), &err);
            CHECK(BURROW_OK(err) && bytes_eq(got, words));
        }
    }

    /* A stream that needs a dictionary it is not given fails in Reset. */
    bytes_reader_reset(&br, dict_stream);
    err = zlib_resetter_reset(rs, bytes_reader_as_io_reader(&br), no_dict);
    CHECK(same_error(err, zlib_err_dictionary));
    Byte one[1];
    CHECK_INT_EQ(
        (int)zr.vt->reader.read(zr.data, slice_from(one, 1, 1, TYPE_BYTE), &err), 0);
    CHECK(same_error(err, zlib_err_dictionary));

    /* And it recovers on the next Reset. */
    bytes_reader_reset(&br, plain_stream);
    CHECK(BURROW_OK(zlib_resetter_reset(rs, bytes_reader_as_io_reader(&br), no_dict)));
    Slice got = io_read_all(a, io_read_closer_as_io_reader(zr), &err);
    CHECK(BURROW_OK(err) && bytes_eq(got, words));

    zlib_reader_free(zr);
    CHECK(zlib_reader_as_resetter((IoReadCloser){NULL, NULL}).vt == NULL);
    zlib_reader_free((IoReadCloser){NULL, NULL});
    arena_free(&ar);
}

/* ------------------------------------------------------------ writer_test.go */

/* Compresses b0 at level with dictionary d, reads it back through a reader
 * with only Read, and compares. */
static void test_level_dict(TestingT *t, const char *fn, Slice b0, Int level,
                            Slice dict) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice z = deflate_all(a, level, dict, b0);
    Error err;
    Slice b1 = inflate_all(a, z, dict, true, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s (level=%d, dict=%d): %s", fn, (int)level,
                           (int)dict.len, error_text(err));
    } else if (b0.len != b1.len) {
        testing_t_errorf_v(t, "%s (level=%d, dict=%d): length mismatch %d versus %d",
                           fn, (int)level, (int)dict.len, (int)b0.len, (int)b1.len);
    } else {
        for (Int i = 0; i < b0.len; i++) {
            if (((Byte *)b0.p)[i] != ((Byte *)b1.p)[i]) {
                testing_t_errorf_v(t, "%s (level=%d): mismatch at %d", fn, (int)level,
                                   (int)i);
                break;
            }
        }
    }
    arena_free(&ar);
}

static void TestWriter(TestingT *t) {
    Slice b = BS("test a reasonable sized string that can be compressed");
    test_level_dict(t, "#0", b, ZLIB_DEFAULT_COMPRESSION, no_dict);
    test_level_dict(t, "#0", b, ZLIB_NO_COMPRESSION, no_dict);
    test_level_dict(t, "#0", b, ZLIB_HUFFMAN_ONLY, no_dict);
    for (Int level = ZLIB_BEST_SPEED; level <= ZLIB_BEST_COMPRESSION; level++)
        test_level_dict(t, "#0", b, level, no_dict);
}

static const char *const file_names[] = {"gettysburg", "words", "noise"};

static void test_files(TestingT *t, Slice dict) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 3; i++) {
        Slice b = zlib_input(a, i);
        test_level_dict(t, file_names[i], b, ZLIB_DEFAULT_COMPRESSION, dict);
        test_level_dict(t, file_names[i], b, ZLIB_NO_COMPRESSION, dict);
        test_level_dict(t, file_names[i], b, ZLIB_HUFFMAN_ONLY, dict);
        for (Int level = ZLIB_BEST_SPEED; level <= ZLIB_BEST_COMPRESSION; level++)
            test_level_dict(t, file_names[i], b, level, dict);
    }
    arena_free(&ar);
}

static void TestWriterBig(TestingT *t) {
    test_files(t, no_dict);
}

static void TestWriterDict(TestingT *t) {
    test_files(t, BS("0123456789."));
}

static void test_file_level_dict_reset(TestingT *t, const char *fn, Slice b0, Int level,
                                       Slice dict) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Compress once. */
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err;
    ZlibWriter *zw =
        dict.p == NULL
            ? zlib_new_writer_level(a, bytes_buffer_as_io_writer(&buf), level, &err)
            : zlib_new_writer_level_dict(a, bytes_buffer_as_io_writer(&buf), level,
                                         dict, &err);
    if (BURROW_OK(err))
        zlib_writer_write(zw, b0, &err);
    if (BURROW_OK(err))
        err = zlib_writer_close(zw);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s (level=%d): %s", fn, (int)level, error_text(err));
        arena_free(&ar);
        return;
    }
    Slice out = bytes_buffer_bytes(&buf);

    /* Reset and compress again. */
    BytesBuffer buf2 = BYTES_BUFFER(a);
    zlib_writer_reset(zw, bytes_buffer_as_io_writer(&buf2));
    zlib_writer_write(zw, b0, &err);
    if (BURROW_OK(err))
        err = zlib_writer_close(zw);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s (level=%d): %s", fn, (int)level, error_text(err));
    } else {
        Slice out2 = bytes_buffer_bytes(&buf2);
        if (!bytes_eq(out, out2))
            testing_t_errorf_v(t,
                               "%s (level=%d): different output after reset (got %d "
                               "bytes, expected %d",
                               fn, (int)level, (int)out2.len, (int)out.len);
    }
    zlib_writer_free(zw);
    arena_free(&ar);
}

static void TestWriterReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice dictionary = BS("0123456789.");
    for (int i = 0; i < 3; i++) {
        Slice b = zlib_input(a, i);
        const char *fn = file_names[i];
        test_file_level_dict_reset(t, fn, b, ZLIB_NO_COMPRESSION, no_dict);
        test_file_level_dict_reset(t, fn, b, ZLIB_DEFAULT_COMPRESSION, no_dict);
        test_file_level_dict_reset(t, fn, b, ZLIB_HUFFMAN_ONLY, no_dict);
        test_file_level_dict_reset(t, fn, b, ZLIB_NO_COMPRESSION, dictionary);
        test_file_level_dict_reset(t, fn, b, ZLIB_DEFAULT_COMPRESSION, dictionary);
        test_file_level_dict_reset(t, fn, b, ZLIB_HUFFMAN_ONLY, dictionary);
        for (Int level = ZLIB_BEST_SPEED; level <= ZLIB_BEST_COMPRESSION; level++)
            test_file_level_dict_reset(t, fn, b, level, no_dict);
    }
    arena_free(&ar);
}

static void TestWriterDictIsUsed(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice input = BS("Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do "
                     "eiusmod tempor incididunt ut labore et dolore magna aliqua.");
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err;
    ZlibWriter *compressor = zlib_new_writer_level_dict(
        a, bytes_buffer_as_io_writer(&buf), ZLIB_BEST_COMPRESSION, input, &err);
    if (compressor == NULL) {
        testing_t_errorf_v(t, "error in NewWriterLevelDict: %s", error_text(err));
        arena_free(&ar);
        return;
    }
    zlib_writer_write(compressor, input, &err);
    zlib_writer_close(compressor);
    const Int expected_max_size = 25;
    Slice output = bytes_buffer_bytes(&buf);
    if (output.len > expected_max_size)
        testing_t_errorf_v(t,
                           "result too large (got %d, want <= %d bytes). Is the "
                           "dictionary being used?",
                           (int)output.len, (int)expected_max_size);
    zlib_writer_free(compressor);
    arena_free(&ar);
}

/* ------------------------------------------------------------ example_test.go */

static const Byte example_stream[] = {120, 156, 0,   13, 0,  242, 255, 104, 101,
                                      108, 108, 111, 44, 32, 119, 111, 114, 108,
                                      100, 10,  3,   0,  33, 231, 4,   147};

static void TestExampleNewWriter(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    ZlibWriter *w = zlib_new_writer(a, bytes_buffer_as_io_writer(&b));
    Error err;
    zlib_writer_write(w, BS("hello, world\n"), &err);
    CHECK(BURROW_OK(err));
    CHECK(BURROW_OK(zlib_writer_close(w)));
    Slice got = bytes_buffer_bytes(&b);
    CHECK(bytes_eq(got, slice_from((void *)(uintptr_t)example_stream,
                                   (Int)sizeof example_stream,
                                   (Int)sizeof example_stream, TYPE_BYTE)));
    zlib_writer_free(w);
    arena_free(&ar);
}

static void TestExampleNewReader(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Slice got = inflate_all(a,
                            slice_from((void *)(uintptr_t)example_stream,
                                       (Int)sizeof example_stream,
                                       (Int)sizeof example_stream, TYPE_BYTE),
                            no_dict, false, &err);
    CHECK(BURROW_OK(err));
    CHECK(bytes_eq(got, BS("hello, world\n")));
    arena_free(&ar);
}

/* ------------------------------------------------------------ burrow's own */

/* What Go's writer makes, byte for byte, at every level with every kind of
 * dictionary, and each of those reads back. The few that Go gets wrong, which
 * the generator marks, come out without the dictionary Go sent as data. */
static void TestGoVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    int last = -1;
    Slice in = no_dict;
    for (size_t i = 0; i < sizeof zlib_vectors / sizeof zlib_vectors[0]; i++) {
        const ZlibVector *v = &zlib_vectors[i];
        if (v->input != last) {
            in = zlib_input(a, v->input);
            last = v->input;
        }
        Slice dict = zlib_dict_of(v->dict);
        Slice got = deflate_all(a, v->level, dict, in);
        char hex[65];
        sha_hex(got, hex);
        if (v->go_bug) {
            /* Go sent the dictionary out in a stored block as well, so the
             * right output is exactly that much shorter. */
            if (got.len != v->len - dict.len || strcmp(hex, v->sha256) == 0)
                testing_t_errorf_v(
                    t, "input %d, level %d, dict %d: got %d bytes, want %d", v->input,
                    v->level, v->dict, (int)got.len, (int)(v->len - dict.len));
        } else if (got.len != v->len || strcmp(hex, v->sha256) != 0) {
            testing_t_errorf_v(t,
                               "input %d, level %d, dict %d: got %d bytes, Go made %d",
                               v->input, v->level, v->dict, (int)got.len, (int)v->len);
        }
        Error err;
        Slice back = inflate_all(a, got, dict, v->input % 2 == 0, &err);
        if (BURROW_FAILED(err) || !bytes_eq(back, in))
            testing_t_errorf_v(t, "input %d, level %d, dict %d: does not read back: %s",
                               v->input, v->level, v->dict, error_text(err));
    }
    arena_free(&ar);
}

/* The header's level bits, the dictionary flag and its checksum. */
static void TestWriterHeader(TestingT *t) {
    /* Levels -2 to 9. */
    static const Byte want[12] = {0x01, 0x9c, 0x01, 0x01, 0x5e, 0x5e,
                                  0x5e, 0x5e, 0x9c, 0xda, 0xda, 0xda};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int level = -2; level <= 9; level++) {
        Slice z = deflate_all(a, level, no_dict, no_dict);
        Byte w1 = want[level + 2];
        CHECK(((Byte *)z.p)[0] == 0x78);
        if (((Byte *)z.p)[1] != w1)
            testing_t_errorf_v(t, "level %d: header byte 1 is %#x, want %#x",
                               (int)level, ((Byte *)z.p)[1], w1);
    }
    Slice dict = BS("0123456789.");
    Slice z = deflate_all(a, 6, dict, no_dict);
    CHECK(((Byte *)z.p)[1] == 0xbb);
    uint32_t sum = adler32_checksum(dict);
    CHECK(((uint32_t)((Byte *)z.p)[2] << 24 | (uint32_t)((Byte *)z.p)[3] << 16 |
           (uint32_t)((Byte *)z.p)[4] << 8 | ((Byte *)z.p)[5]) == sum);
    arena_free(&ar);
}

static void TestInvalidLevel(TestingT *t) {
    static const Int levels[] = {-3, 10, 100, -2147483647 - 1};
    static const char *const want[] = {
        "zlib: invalid compression level: -3",
        "zlib: invalid compression level: 10",
        "zlib: invalid compression level: 100",
        "zlib: invalid compression level: -2147483648",
    };
    for (int i = 0; i < 4; i++) {
        Error err;
        ZlibWriter *w =
            zlib_new_writer_level(heap_allocator(), io_discard, levels[i], &err);
        CHECK(w == NULL);
        CHECK(text_is(error_text(err), want[i]));
        w = zlib_new_writer_level_dict(heap_allocator(), io_discard, levels[i], BS("d"),
                                       &err);
        CHECK(w == NULL);
        CHECK(text_is(error_text(err), want[i]));
    }
}

/* A failed write anywhere, in the header, the compressed data or the
 * checksum, comes back from that call and every call after it, and Reset
 * clears it. */
static void TestWriterErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = zlib_input(a, 1);
    Slice dict = BS("0123456789.");
    for (int d = 0; d < 2; d++) {
        for (int fail_at = 0; fail_at < 8; fail_at++) {
            FailWriter fw = {fail_at};
            Error err;
            ZlibWriter *zw = zlib_new_writer_level_dict(
                a, (IoWriter){&fail_writer_vt, &fw}, 6, d ? dict : no_dict, &err);
            CHECK(zw != NULL);
            zlib_writer_write(zw, in, &err);
            Error ferr = zlib_writer_flush(zw);
            Error cerr = zlib_writer_close(zw);
            if (!same_error(cerr, err_io))
                testing_t_errorf_v(t, "dict %d, fail at %d: Close gave %s", d, fail_at,
                                   error_text(cerr));
            if (BURROW_FAILED(err) && !same_error(err, err_io))
                testing_t_errorf_v(t, "dict %d, fail at %d: Write gave %s", d, fail_at,
                                   error_text(err));
            if (BURROW_FAILED(ferr) && !same_error(ferr, err_io))
                testing_t_errorf_v(t, "dict %d, fail at %d: Flush gave %s", d, fail_at,
                                   error_text(ferr));
            Int n = zlib_writer_write(zw, BS("more"), &err);
            CHECK(n == 0 && same_error(err, err_io));

            BytesBuffer buf = BYTES_BUFFER(a);
            zlib_writer_reset(zw, bytes_buffer_as_io_writer(&buf));
            zlib_writer_write(zw, in, &err);
            CHECK(BURROW_OK(err));
            CHECK(BURROW_OK(zlib_writer_close(zw)));
            CHECK(bytes_eq(bytes_buffer_bytes(&buf),
                           deflate_all(a, 6, d ? dict : no_dict, in)));
            zlib_writer_free(zw);
        }
    }
    arena_free(&ar);
}

/* An empty Write writes the header and nothing else, and gives (0, nil). */
static void TestWriterEmptyWrite(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZlibWriter *zw = zlib_new_writer(a, bytes_buffer_as_io_writer(&buf));
    Error err;
    CHECK_INT_EQ((int)zlib_writer_write(zw, no_dict, &err), 0);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ((int)bytes_buffer_len(&buf), 2);
    CHECK(BURROW_OK(zlib_writer_flush(zw)));
    CHECK(BURROW_OK(zlib_writer_close(zw)));
    Slice back = inflate_all(a, bytes_buffer_bytes(&buf), no_dict, false, &err);
    CHECK(BURROW_OK(err) && back.len == 0);
    zlib_writer_free(zw);
    arena_free(&ar);
}

/* The writer as an IoWriter and an IoWriteCloser, and freeing NULL. */
static void TestWriterInterfaces(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    ZlibWriter *w = zlib_new_writer(a, bytes_buffer_as_io_writer(&buf));
    IoWriteCloser wc = zlib_writer_as_io_write_closer(w);
    IoWriter iw = io_write_closer_as_io_writer(wc);
    Error err;
    CHECK_INT_EQ((int)iw.vt->write(iw.data, BS("hello, world\n"), &err), 13);
    CHECK(BURROW_OK(wc.vt->closer.close(wc.data)));
    CHECK(bytes_eq(bytes_buffer_bytes(&buf),
                   slice_from((void *)(uintptr_t)example_stream,
                              (Int)sizeof example_stream, (Int)sizeof example_stream,
                              TYPE_BYTE)));
    CHECK(zlib_writer_as_io_writer(w).data == w);
    CHECK(wc.vt->writer.self_type == TYPE_ZLIB_WRITER);
    zlib_writer_free(w);
    zlib_writer_free(NULL);
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
    Slice in = zlib_input(scratch, 0);
    Slice dict = BS("0123456789.");
    Slice z = deflate_all(scratch, 6, dict, in);

    /* The reader, from a source without ReadByte so that it needs a buffer. */
    bool done = false;
    for (int budget = 0; budget < 64 && !done; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesReader br;
        bytes_reader_reset(&br, z);
        PlainReader pr = {bytes_reader_as_io_reader(&br)};
        Error err;
        IoReadCloser rc =
            zlib_new_reader_dict(&al, (IoReader){&plain_reader_vt, &pr}, dict, &err);
        if (rc.vt == NULL) {
            if (!same_error(err, burrow_err_out_of_memory))
                testing_t_errorf_v(t, "reader, budget %d: %s", budget, error_text(err));
        } else {
            /* The decompressor builds its Huffman tables as it reads, so
             * running out can come from reading too. */
            Slice got = io_read_all(scratch, io_read_closer_as_io_reader(rc), &err);
            if (BURROW_OK(err)) {
                done = true;
                CHECK(bytes_eq(got, in));
            } else if (!same_error(err, burrow_err_out_of_memory)) {
                testing_t_errorf_v(t, "reader, budget %d: read: %s", budget,
                                   error_text(err));
            }
            zlib_reader_free(rc);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "reader, budget %d: %d bytes leaked", budget,
                               (int)b.live);
    }
    CHECK(done);

    /* The writer, which only allocates the compressor on the first write. */
    done = false;
    for (int budget = 0; budget < 64 && !done; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesBuffer out = BYTES_BUFFER(scratch);
        Error err;
        ZlibWriter *w = zlib_new_writer_level_dict(&al, bytes_buffer_as_io_writer(&out),
                                                   6, dict, &err);
        if (w == NULL) {
            if (!same_error(err, burrow_err_out_of_memory))
                testing_t_errorf_v(t, "writer, budget %d: %s", budget, error_text(err));
        } else {
            zlib_writer_write(w, in, &err);
            Error cerr = zlib_writer_close(w);
            if (BURROW_OK(err) && BURROW_OK(cerr)) {
                done = true;
                CHECK(bytes_eq(bytes_buffer_bytes(&out), z));
            } else if (!same_error(err, burrow_err_out_of_memory) ||
                       !same_error(cerr, burrow_err_out_of_memory)) {
                testing_t_errorf_v(t, "writer, budget %d: %s, %s", budget,
                                   error_text(err), error_text(cerr));
            }
            zlib_writer_free(w);
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
    X(TestReaderStopsAtChecksum)                                                       \
    X(TestReaderReset)                                                                 \
    X(TestWriter)                                                                      \
    X(TestWriterBig)                                                                   \
    X(TestWriterDict)                                                                  \
    X(TestWriterReset)                                                                 \
    X(TestWriterDictIsUsed)                                                            \
    X(TestExampleNewWriter)                                                            \
    X(TestExampleNewReader)                                                            \
    X(TestGoVectors)                                                                   \
    X(TestWriterHeader)                                                                \
    X(TestInvalidLevel)                                                                \
    X(TestWriterErrors)                                                                \
    X(TestWriterEmptyWrite)                                                            \
    X(TestWriterInterfaces)                                                            \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
