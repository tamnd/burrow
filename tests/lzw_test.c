/* Derived from Go's src/compress/lzw/reader_test.go and writer_test.go.
 * Go source: go1.27.1.
 *
 * Go's writer tests read gettysburg.txt, e.txt and pi.txt from testdata.
 * gettysburg.txt is in tests/flate_test_gen.h already, and words and digits
 * made the same way on both sides stand in for the other two. Go pushes the
 * data through an io.Pipe with the writer in a goroutine; here the writer
 * fills a buffer first, in the same 4096 byte writes, and a reader with only
 * Read takes it back. tests/lzw_test_gen.h, from tools/gen-lzw-tests.sh, has
 * the SHA-256 of what Go's writer makes of each input, so the output is checked
 * byte for byte as well as by reading it back, and what Go's reader makes of
 * noise and of streams cut short.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/compress/lzw.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/base64.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "flate_test_gen.h"
#include "lzw_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static Slice sub(Slice b, Int lo, Int hi) {
    return slice_from((Byte *)b.p + lo, hi - lo, hi - lo, TYPE_BYTE);
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

static inline uint64_t lcg(uint64_t x) {
    return x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
}

/* The words tools/gen-lzw-tests.sh compresses, made the same way. */
static Slice lzw_words(Alloc *a, Int n) {
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
        x = lcg(x);
        const char *w = vocab[(x >> 33) % nv];
        size_t k = strlen(w);
        for (size_t i = 0; i < k && len < n; i++)
            p[len++] = (Byte)w[i];
        if (len < n)
            p[len++] = x >> 60 == 0 ? '\n' : ' ';
    }
    return slice_from(p, n, n, TYPE_BYTE);
}

/* Bytes from the same generator, seeded, the top byte of each step, masked. */
static Slice lzw_noise(Alloc *a, Int n, uint64_t seed, Byte mask) {
    Slice b = make_bytes(a, n);
    uint64_t x = seed;
    for (Int i = 0; i < n; i++) {
        x = lcg(x);
        ((Byte *)b.p)[i] = (Byte)(x >> 56) & mask;
    }
    return b;
}

/* Decimal digits from the same generator, like e.txt and pi.txt. */
static Slice lzw_digits(Alloc *a, Int n) {
    Slice b = make_bytes(a, n);
    uint64_t x = 3;
    for (Int i = 0; i < n; i++) {
        x = lcg(x);
        ((Byte *)b.p)[i] = (Byte)('0' + (x >> 33) % 10);
    }
    return b;
}

/* The inputs the generator numbers. */
static Slice lzw_input(Alloc *a, int id, int lit_width) {
    switch (id) {
    case 0:
        return gen_bytes(a, gettysburg);
    case 1:
        return lzw_words(a, 100000);
    case 2:
        return lzw_noise(a, 70000, 7, (Byte)((1U << lit_width) - 1));
    case 3:
        return make_bytes(a, 0);
    case 4:
        return BS("TOBEORNOTTOBEORTOBEORNOT");
    case 5:
        return make_bytes(a, 20000);
    default:
        return lzw_digits(a, 100000);
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

/* Compresses in, in writes of chunk bytes, and a Close. */
static Slice compress(Alloc *a, Slice in, LzwOrder order, int lit_width, Int chunk) {
    BytesBuffer out = BYTES_BUFFER(a);
    LzwWriter *w = lzw_new_writer(a, bytes_buffer_as_io_writer(&out), order, lit_width);
    if (w == NULL)
        abort();
    Error err;
    for (Int i = 0; i < in.len; i += chunk) {
        Int end = i + chunk < in.len ? i + chunk : in.len;
        Int n = lzw_writer_write(w, sub(in, i, end), &err);
        if (n != end - i || BURROW_FAILED(err)) {
            fprintf(stderr, "lzw_writer_write: %.*s\n", (int)error_text(err).len,
                    (const char *)error_text(err).p);
            abort();
        }
    }
    if (BURROW_FAILED(lzw_writer_close(w)))
        abort();
    lzw_writer_free(w);
    return bytes_buffer_bytes(&out);
}

/* io.ReadAll of a reader over in, through a reader with only Read when plain
 * is set. */
static Slice decompress(Alloc *a, Slice in, LzwOrder order, int lit_width, bool plain,
                        Error *err) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    PlainReader pr = {bytes_reader_as_io_reader(&br)};
    IoReader src = plain ? (IoReader){&plain_reader_vt, &pr} : pr.r;
    LzwReader *r = lzw_new_reader(a, src, order, lit_width);
    if (r == NULL)
        abort();
    Slice out = io_read_all(a, lzw_reader_as_io_reader(r), err);
    if (BURROW_FAILED(lzw_reader_close(r)))
        abort();
    lzw_reader_free(r);
    return out;
}

/* ------------------------------------------------------------ reader_test.go */

typedef struct LzwTest {
    const char *desc;
    LzwOrder order;
    int lit_width;
    const char *raw;
    size_t raw_len;
    const char *compressed;
    size_t compressed_len;
    bool unexpected_eof;
} LzwTest;

#define S(lit) lit, sizeof(lit) - 1

static const LzwTest lzw_tests[] = {
    {"empty;LSB;8", LZW_LSB, 8, S(""), S("\x01\x01"), false},
    {"empty;MSB;8", LZW_MSB, 8, S(""), S("\x80\x80"), false},
    {"tobe;LSB;7", LZW_LSB, 7, S("TOBEORNOTTOBEORTOBEORNOT"),
     S("\x54\x4f\x42\x45\x4f\x52\x4e\x4f\x54\x82\x84\x86\x8b\x85\x87\x89\x81"), false},
    {"tobe;LSB;8", LZW_LSB, 8, S("TOBEORNOTTOBEORTOBEORNOT"),
     S("\x54\x9e\x08\x29\xf2\x44\x8a\x93\x27\x54\x04\x12\x34\xb8\xb0\xe0\xc1\x84\x01"
       "\x01"),
     false},
    {"tobe;MSB;7", LZW_MSB, 7, S("TOBEORNOTTOBEORTOBEORNOT"),
     S("\x54\x4f\x42\x45\x4f\x52\x4e\x4f\x54\x82\x84\x86\x8b\x85\x87\x89\x81"), false},
    {"tobe;MSB;8", LZW_MSB, 8, S("TOBEORNOTTOBEORTOBEORNOT"),
     S("\x2a\x13\xc8\x44\x52\x79\x48\x9c\x4f\x2a\x40\xa0\x90\x68\x5c\x16\x0f\x09\x80"
       "\x80"),
     false},
    {"tobe-truncated;LSB;8", LZW_LSB, 8, S("TOBEORNOTTOBEORTOBEORNOT"),
     S("\x54\x9e\x08\x29\xf2\x44\x8a\x93\x27\x54\x04"), true},
    /* This example comes from
     * https://en.wikipedia.org/wiki/Graphics_Interchange_Format. */
    {"gif;LSB;8", LZW_LSB, 8,
     S("\x28\xff\xff\xff\x28\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"),
     S("\x00\x51\xfc\x1b\x28\x70\xa0\xc1\x83\x01\x01"), false},
    /* This example comes from
     * http://compgroups.net/comp.lang.ruby/Decompressing-LZW-compression-from-PDF-file */
    {"pdf;MSB;8", LZW_MSB, 8, S("-----A---B"),
     S("\x80\x0b\x60\x50\x22\x0c\x0c\x85\x01"), false},
};

static Slice test_bytes(const char *p, size_t n) {
    return slice_from((void *)(uintptr_t)p, (Int)n, (Int)n, TYPE_BYTE);
}

/* Go's TestReader, through a strings.Reader, which has ReadByte, and through a
 * reader with only Read. */
static void TestReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof lzw_tests / sizeof lzw_tests[0]; i++) {
        const LzwTest *tt = &lzw_tests[i];
        Slice raw = test_bytes(tt->raw, tt->raw_len);
        for (int plain = 0; plain < 2; plain++) {
            StringsReader sr;
            strings_reader_reset(
                &sr, str_from_bytes(tt->compressed, (Int)tt->compressed_len));
            PlainReader pr = {strings_reader_as_io_reader(&sr)};
            IoReader src = plain ? (IoReader){&plain_reader_vt, &pr} : pr.r;
            LzwReader *rc = lzw_new_reader(a, src, tt->order, tt->lit_width);
            BytesBuffer b = BYTES_BUFFER(a);
            Error err;
            int64_t n = io_copy(a, bytes_buffer_as_io_writer(&b),
                                lzw_reader_as_io_reader(rc), &err);
            Slice s = bytes_buffer_bytes(&b);
            if (BURROW_FAILED(err)) {
                if (!tt->unexpected_eof || !same_error(err, io_err_unexpected_eof))
                    testing_t_errorf_v(t, "%s: io.Copy: %s", tt->desc, error_text(err));
                /* Even if the input is truncated, we should still return the
                 * partial decoded result. */
                if (n == 0 || s.len > raw.len || memcmp(s.p, raw.p, (size_t)s.len) != 0)
                    testing_t_errorf_v(t, "%s: got %d bytes, want a non-empty prefix",
                                       tt->desc, (int)n);
            } else if (tt->unexpected_eof || !bytes_eq(s, raw)) {
                testing_t_errorf_v(t, "%s: got %d-byte %q want %d-byte %q", tt->desc,
                                   (int)n, s, (int)raw.len, raw);
            }
            lzw_reader_close(rc);
            lzw_reader_free(rc);
        }
    }
    arena_free(&ar);
}

static void TestReaderReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof lzw_tests / sizeof lzw_tests[0]; i++) {
        const LzwTest *tt = &lzw_tests[i];
        StringsReader sr;
        strings_reader_reset(&sr,
                             str_from_bytes(tt->compressed, (Int)tt->compressed_len));
        LzwReader *rc = lzw_new_reader(a, strings_reader_as_io_reader(&sr), tt->order,
                                       tt->lit_width);
        Error err;
        Slice b1 = io_read_all(a, lzw_reader_as_io_reader(rc), &err);
        if (BURROW_FAILED(err)) {
            if (!tt->unexpected_eof || !same_error(err, io_err_unexpected_eof))
                testing_t_errorf_v(t, "%s: io.Copy: %s", tt->desc, error_text(err));
            if (b1.len == 0)
                testing_t_errorf_v(t, "%s: got no bytes, want a non-empty prefix",
                                   tt->desc);
            lzw_reader_free(rc);
            continue;
        }

        strings_reader_reset(&sr,
                             str_from_bytes(tt->compressed, (Int)tt->compressed_len));
        lzw_reader_reset(rc, strings_reader_as_io_reader(&sr), tt->order,
                         tt->lit_width);
        Slice b2 = io_read_all(a, lzw_reader_as_io_reader(rc), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: io.Copy: %s want nil", tt->desc,
                               error_text(err));
        else if (!bytes_eq(b1, b2))
            testing_t_errorf_v(t, "%s: bytes read were not the same", tt->desc);
        lzw_reader_free(rc);
    }
    arena_free(&ar);
}

static Int dev_zero_read(void *self, Slice p, Error *err) {
    (void)self;
    if (p.len > 0)
        memset(p.p, 0, (size_t)p.len);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoReaderVT dev_zero_vt = {NULL, dev_zero_read};

/* Go's test also checks that the reader's hi field never goes down, which it
 * can read from inside the package. What that guards against is hi passing
 * overflow, which panics in Go, so here it is enough that reading keeps going. */
static void TestHiCodeDoesNotOverflow(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    int zero = 0;
    LzwReader *r = lzw_new_reader(a, (IoReader){&dev_zero_vt, &zero}, LZW_LSB, 8);
    Slice buf = make_bytes(a, 1024);
    for (int i = 0; i < 100; i++) {
        Error err;
        io_read_full(lzw_reader_as_io_reader(r), buf, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "i=%d: %s", i, error_text(err));
            break;
        }
    }
    lzw_reader_free(r);
    arena_free(&ar);
}

/* TestNoLongerSavingPriorExpansions tests the decoder state when codes other
 * than clear codes continue to be seen after hi and width reach their maximum
 * values (4095 and 12), i.e. after we no longer save prior expansions. In
 * particular, it tests seeing the highest possible code, 4095. Go's comment
 * works through the bits. */
static void TestNoLongerSavingPriorExpansions(TestingT *t) {
    /* Iterations is used to calculate how many input bits are needed to get
     * hi and width up to their maximum. */
    static const int iterations[][2] = {
        /* The final term is 257, not 256, as NewReader initializes hi to
         * clear+1 and the clear code is 256. */
        {9, 512 - 257},
        {10, 1024 - 512},
        {11, 2048 - 1024},
        {12, 4096 - 2048},
    };
    int n_codes = 0, n_bits = 0;
    for (int i = 0; i < 4; i++) {
        n_codes += iterations[i][1];
        n_bits += iterations[i][1] * iterations[i][0];
    }
    CHECK(n_codes == 3839);
    CHECK(n_bits == 43255);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = make_bytes(a, 5406 + 4);
    Byte *p = (Byte *)in.p;
    p[5406] = 0x80;
    p[5407] = 0xff;
    p[5408] = 0x0f;
    p[5409] = 0x08;

    BytesReader br;
    bytes_reader_reset(&br, in);
    LzwReader *r = lzw_new_reader(a, bytes_reader_as_io_reader(&br), LZW_LSB, 8);
    Error err;
    int64_t n_decoded = io_copy(a, io_discard, lzw_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Copy: %s", error_text(err));
    /* n_decoded should be 3841: 3839 literal codes and then 2 decoded bytes
     * from 1 non-literal code. The EOF code contributes 0 decoded bytes. */
    else if (n_decoded != n_codes + 2)
        testing_t_errorf_v(t, "nDecoded: got %d, want %d", (int)n_decoded, n_codes + 2);
    lzw_reader_free(r);
    arena_free(&ar);
}

/* ------------------------------------------------------------ writer_test.go */

/* testFile, with the writer's 4096 byte writes and the reader reading from
 * something with only Read, as it would from Go's pipe. */
static void test_file(TestingT *t, Alloc *a, int input, LzwOrder order, int lit_width) {
    Slice golden = lzw_input(a, input, lit_width);
    Slice z = compress(a, golden, order, lit_width, 4096);
    Error err;
    Slice b1 = decompress(a, z, order, lit_width, true, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "input %d (order=%d litWidth=%d): %s", input, (int)order,
                           lit_width, error_text(err));
        return;
    }
    if (b1.len != golden.len) {
        testing_t_errorf_v(t,
                           "input %d (order=%d litWidth=%d): length mismatch %d != %d",
                           input, (int)order, lit_width, (int)b1.len, (int)golden.len);
        return;
    }
    for (Int i = 0; i < golden.len; i++) {
        if (((Byte *)b1.p)[i] != ((Byte *)golden.p)[i]) {
            testing_t_errorf_v(t, "input %d (order=%d litWidth=%d): mismatch at %d",
                               input, (int)order, lit_width, (int)i);
            return;
        }
    }
}

static void TestWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* gettysburg.txt, then words and digits for e.txt and pi.txt. */
    static const int inputs[] = {0, 1, 6};
    for (size_t i = 0; i < 3; i++) {
        for (LzwOrder order = LZW_LSB; order <= LZW_MSB; order++) {
            /* The test data "2.71828 etcetera" is ASCII text requiring at
             * least 6 bits. */
            for (int lit_width = 6; lit_width <= 8; lit_width++) {
                if (inputs[i] != 6 && lit_width == 6)
                    continue;
                test_file(t, a, inputs[i], order, lit_width);
            }
        }
    }
    arena_free(&ar);
}

static void TestWriterReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (LzwOrder order = LZW_LSB; order <= LZW_MSB; order++) {
        for (int lit_width = 6; lit_width <= 8; lit_width++) {
            Slice data =
                lit_width == 6 ? BS("\x01\x02\x03") : BS("lorem ipsum dolor sit amet");
            BytesBuffer buf = BYTES_BUFFER(a);
            LzwWriter *w =
                lzw_new_writer(a, bytes_buffer_as_io_writer(&buf), order, lit_width);
            Error err;
            lzw_writer_write(w, data, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "write: %s", error_text(err));
            err = lzw_writer_close(w);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "close: %s", error_text(err));
            Slice b1 = bytes_clone(a, bytes_buffer_bytes(&buf));
            bytes_buffer_reset(&buf);

            lzw_writer_reset(w, bytes_buffer_as_io_writer(&buf), order, lit_width);
            lzw_writer_write(w, data, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "write: %s", error_text(err));
            err = lzw_writer_close(w);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "close: %s", error_text(err));
            if (!bytes_eq(b1, bytes_buffer_bytes(&buf)))
                testing_t_errorf_v(t,
                                   "order %d litWidth %d: bytes written were not same",
                                   (int)order, lit_width);
            lzw_writer_free(w);
        }
    }
    arena_free(&ar);
}

static void TestWriterReturnValues(TestingT *t) {
    (void)t;
    LzwWriter *w = lzw_new_writer(heap_allocator(), io_discard, LZW_LSB, 8);
    Error err;
    Int n = lzw_writer_write(w, BS("asdf"), &err);
    CHECK(n == 4 && BURROW_OK(err));
    lzw_writer_free(w);
}

static void TestSmallLitWidth(TestingT *t) {
    LzwWriter *w = lzw_new_writer(heap_allocator(), io_discard, LZW_LSB, 2);
    Error err;
    lzw_writer_write(w, BS("\x03"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "write a byte < 1<<2: %s", error_text(err));
    lzw_writer_write(w, BS("\x04"), &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "write a byte >= 1<<2: got nil error, want non-nil");
    else
        CHECK(text_is(error_text(err), "lzw: input byte too large for the litWidth"));
    lzw_writer_free(w);
}

static void TestStartsWithClearCode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* A literal width of 7 bits means that the code width starts at 8 bits,
     * which makes it easier to visually inspect the output (provided that the
     * output is short so codes don't get longer). Each byte is a code:
     *  - ASCII bytes are literal codes,
     *  - 0x80 is the clear code,
     *  - 0x81 is the end code.
     *  - 0x82 and above are copy codes (unused in this test case). */
    for (int empty = 0; empty < 2; empty++) {
        BytesBuffer buf = BYTES_BUFFER(a);
        LzwWriter *w = lzw_new_writer(a, bytes_buffer_as_io_writer(&buf), LZW_LSB, 7);
        Error err;
        if (!empty)
            lzw_writer_write(w, BS("Hi"), &err);
        lzw_writer_close(w);
        Slice want = empty ? BS("\x80\x81") : BS("\x80Hi\x81");
        if (!bytes_eq(bytes_buffer_bytes(&buf), want))
            testing_t_errorf_v(t, "empty=%d: got %q, want %q", empty,
                               bytes_buffer_bytes(&buf), want);
        lzw_writer_free(w);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------ burrow's own */

/* What Go's writer makes, byte for byte, whole and in writes of a few sizes,
 * and each reads back. */
static void TestGoVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Int chunks[] = {1 << 30, 1, 7, 4096};
    for (size_t i = 0; i < sizeof lzw_vectors / sizeof lzw_vectors[0]; i++) {
        const LzwVector *v = &lzw_vectors[i];
        Slice in = lzw_input(a, v->input, v->lit_width);
        for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
            Slice z = compress(a, in, v->order, v->lit_width, chunks[c]);
            char hex[65];
            sha_hex(z, hex);
            if (z.len != v->len || strcmp(hex, v->sha256) != 0) {
                testing_t_errorf_v(t,
                                   "input %d order %d width %d chunk %d: %d bytes, "
                                   "want %d the same as Go's",
                                   v->input, v->order, v->lit_width, (int)chunks[c],
                                   (int)z.len, (int)v->len);
                continue;
            }
            if (c > 0)
                continue;
            for (int plain = 0; plain < 2; plain++) {
                Error err;
                Slice got = decompress(a, z, v->order, v->lit_width, plain != 0, &err);
                if (BURROW_FAILED(err) || !bytes_eq(got, in))
                    testing_t_errorf_v(t, "input %d order %d width %d: read back: %s",
                                       v->input, v->order, v->lit_width,
                                       error_text(err));
            }
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* What Go's reader makes of noise and of streams cut short: the same bytes
 * before the same error. */
static void TestGoDecodeVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice streams[2] = {{0}, {0}};
    for (size_t i = 0; i < sizeof lzw_decode_vectors / sizeof lzw_decode_vectors[0];
         i++) {
        const LzwDecodeVector *v = &lzw_decode_vectors[i];
        Slice in;
        if (v->words) {
            if (streams[v->order].p == NULL) {
                Arena *keep = &ar;
                streams[v->order] = compress(arena_allocator(keep),
                                             lzw_words(a, 100000), v->order, 8, 4096);
            }
            in = sub(streams[v->order], 0, v->n);
        } else {
            in = lzw_noise(a, v->n, (uint64_t)v->seed, 0xff);
        }
        for (int plain = 0; plain < 2; plain++) {
            Error err;
            Slice got = decompress(a, in, v->order, v->lit_width, plain != 0, &err);
            char hex[65];
            sha_hex(got, hex);
            bool err_ok = v->err[0] == 0
                              ? BURROW_OK(err)
                              : BURROW_FAILED(err) && text_is(error_text(err), v->err);
            if (got.len != v->len || strcmp(hex, v->sha256) != 0 || !err_ok)
                testing_t_errorf_v(
                    t, "vector %d: %d bytes and %s, want %d and \"%s\" as Go", (int)i,
                    (int)got.len, error_text(err), (int)v->len, v->err);
        }
    }
    arena_free(&ar);
}

/* A bad order or width is reported by the first read, write or close, with
 * Go's messages. */
static void TestBadOrderAndWidth(TestingT *t) {
    static const struct {
        LzwOrder order;
        Int lit_width;
        const char *want;
    } cases[] = {
        {2, 8, "lzw: unknown order"},
        {-1, 8, "lzw: unknown order"},
        {LZW_LSB, 1, "lzw: litWidth 1 out of range"},
        {LZW_MSB, 9, "lzw: litWidth 9 out of range"},
        {LZW_LSB, -3, "lzw: litWidth -3 out of range"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        StringsReader sr;
        strings_reader_reset(&sr, BURROW_S("\x01\x01"));
        LzwReader *r =
            lzw_new_reader(heap_allocator(), strings_reader_as_io_reader(&sr),
                           cases[i].order, cases[i].lit_width);
        Byte buf[4];
        Error err;
        Int n = lzw_reader_read(r, slice_from(buf, 4, 4, TYPE_BYTE), &err);
        if (n != 0 || !text_is(error_text(err), cases[i].want))
            testing_t_errorf_v(t, "case %d: read gave %d, %s", (int)i, (int)n,
                               error_text(err));
        lzw_reader_free(r);

        BytesBuffer out = BYTES_BUFFER(heap_allocator());
        LzwWriter *w = lzw_new_writer(heap_allocator(), bytes_buffer_as_io_writer(&out),
                                      cases[i].order, cases[i].lit_width);
        n = lzw_writer_write(w, BS("a"), &err);
        if (n != 0 || !text_is(error_text(err), cases[i].want))
            testing_t_errorf_v(t, "case %d: write gave %d, %s", (int)i, (int)n,
                               error_text(err));
        err = lzw_writer_close(w);
        if (!text_is(error_text(err), cases[i].want))
            testing_t_errorf_v(t, "case %d: close gave %s", (int)i, error_text(err));
        CHECK(bytes_buffer_len(&out) == 0);
        lzw_writer_free(w);
        bytes_buffer_free(&out);
    }
}

static void TestClosed(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("\x01\x01"));
    LzwReader *r = lzw_new_reader(a, strings_reader_as_io_reader(&sr), LZW_LSB, 8);
    CHECK(BURROW_OK(lzw_reader_close(r)));
    Byte buf[4];
    Error err;
    CHECK(lzw_reader_read(r, slice_from(buf, 4, 4, TYPE_BYTE), &err) == 0);
    CHECK(text_is(error_text(err), "lzw: reader/writer is closed"));
    CHECK(BURROW_OK(lzw_reader_close(r)));
    /* Reset opens it again. */
    strings_reader_reset(&sr, BURROW_S("\x01\x01"));
    lzw_reader_reset(r, strings_reader_as_io_reader(&sr), LZW_LSB, 8);
    CHECK(lzw_reader_read(r, slice_from(buf, 4, 4, TYPE_BYTE), &err) == 0);
    CHECK(same_error(err, io_eof));
    lzw_reader_free(r);

    BytesBuffer out = BYTES_BUFFER(a);
    LzwWriter *w = lzw_new_writer(a, bytes_buffer_as_io_writer(&out), LZW_LSB, 8);
    CHECK(BURROW_OK(lzw_writer_close(w)));
    CHECK(BURROW_OK(lzw_writer_close(w)));
    CHECK(lzw_writer_write(w, BS("a"), &err) == 0);
    CHECK(text_is(error_text(err), "lzw: reader/writer is closed"));
    /* An empty write after an error still gives the error. */
    CHECK(lzw_writer_write(w, BS(""), &err) == 0);
    CHECK(text_is(error_text(err), "lzw: reader/writer is closed"));
    lzw_writer_free(w);
    arena_free(&ar);
}

/* The writer's output only goes through when its bufio.Writer fills up or at
 * Close, and a failure there comes back and sticks. */
static void TestWriterErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = lzw_words(a, 100000);
    for (int fail_at = 0; fail_at < 4; fail_at++) {
        FailWriter fw = {fail_at};
        LzwWriter *w = lzw_new_writer(a, (IoWriter){&fail_writer_vt, &fw}, LZW_LSB, 8);
        Error err = BURROW_NO_ERROR;
        Error first = BURROW_NO_ERROR;
        for (Int i = 0; i < in.len && BURROW_OK(err); i += 1000) {
            lzw_writer_write(w, sub(in, i, i + 1000), &err);
            first = err;
        }
        Error cerr = lzw_writer_close(w);
        if (BURROW_OK(first) && BURROW_OK(cerr))
            testing_t_errorf_v(t, "fail at %d: no error", fail_at);
        if (BURROW_FAILED(first)) {
            CHECK(same_error(first, err_io));
            CHECK(same_error(cerr, err_io));
            Error again;
            CHECK(lzw_writer_write(w, BS("x"), &again) == 0);
            CHECK(same_error(again, err_io));
        } else {
            CHECK(same_error(cerr, err_io));
        }
        lzw_writer_free(w);
    }
    arena_free(&ar);
}

/* A bufio.Writer, or the one in a bufio.ReadWriter, is written to directly,
 * and the output is still Go's. */
static void TestWriterIntoBufio(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = lzw_input(a, 0, 8);
    Slice want = compress(a, in, LZW_MSB, 8, 1 << 30);
    for (int rw = 0; rw < 2; rw++) {
        BytesBuffer out = BYTES_BUFFER(a);
        BufioWriter *bw = bufio_new_writer(a, bytes_buffer_as_io_writer(&out));
        BufioReadWriter brw = {NULL, bw};
        IoWriter dst =
            rw ? bufio_read_writer_as_io_writer(&brw) : bufio_writer_as_io_writer(bw);
        LzwWriter *w = lzw_new_writer(a, dst, LZW_MSB, 8);
        Error err;
        lzw_writer_write(w, in, &err);
        CHECK(BURROW_OK(err));
        CHECK(BURROW_OK(lzw_writer_close(w)));
        CHECK(bytes_eq(bytes_buffer_bytes(&out), want));
        lzw_writer_free(w);
        bufio_writer_free(bw);
    }
    arena_free(&ar);
}

/* The reader takes bytes one at a time from a reader with ReadByte, so it
 * stops right after the end code. */
static void TestReaderStopsAtEnd(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = lzw_input(a, 0, 8);
    Slice z = compress(a, in, LZW_LSB, 8, 1 << 30);
    Slice both = make_bytes(a, z.len + 5);
    memcpy(both.p, z.p, (size_t)z.len);
    memcpy((Byte *)both.p + z.len, "after", 5);
    BytesReader br;
    bytes_reader_reset(&br, both);
    LzwReader *r = lzw_new_reader(a, bytes_reader_as_io_reader(&br), LZW_LSB, 8);
    Error err;
    Slice got = io_read_all(a, lzw_reader_as_io_reader(r), &err);
    CHECK(BURROW_OK(err) && bytes_eq(got, in));
    Slice rest = io_read_all(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(bytes_eq(rest, BS("after")));
    lzw_reader_free(r);
    arena_free(&ar);
}

static void TestInterfaces(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer out = BYTES_BUFFER(a);
    LzwWriter *w = lzw_new_writer(a, bytes_buffer_as_io_writer(&out), LZW_LSB, 8);
    IoWriter iw = lzw_writer_as_io_writer(w);
    IoWriteCloser iwc = lzw_writer_as_io_write_closer(w);
    CHECK(iw.data == w && iw.vt->self_type == TYPE_LZW_WRITER);
    CHECK(iwc.data == w && iwc.vt->writer.self_type == TYPE_LZW_WRITER);
    Error err;
    CHECK(io_write_string(iw, BURROW_S("hello"), &err) == 5 && BURROW_OK(err));
    CHECK(BURROW_OK(iwc.vt->closer.close(iwc.data)));

    BytesReader br;
    bytes_reader_reset(&br, bytes_buffer_bytes(&out));
    LzwReader *r = lzw_new_reader(a, bytes_reader_as_io_reader(&br), LZW_LSB, 8);
    IoReadCloser rc = lzw_reader_as_io_read_closer(r);
    CHECK(rc.data == r && rc.vt->reader.self_type == TYPE_LZW_READER);
    CHECK(lzw_reader_as_io_reader(r).vt->self_type == TYPE_LZW_READER);
    Slice got = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    CHECK(BURROW_OK(err) && bytes_eq(got, BS("hello")));
    CHECK(BURROW_OK(rc.vt->closer.close(rc.data)));
    lzw_writer_free(w);
    lzw_reader_free(r);
    lzw_reader_free(NULL);
    lzw_writer_free(NULL);
    arena_free(&ar);
}

/* An allocator that refuses after a number of allocations and counts what is
 * live, so that nothing leaks when it runs out. */
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
    Slice in = lzw_input(scratch, 0, 8);
    Slice z = compress(scratch, in, LZW_LSB, 8, 1 << 30);
    int made = 0;
    for (int budget = 0; budget < 4; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};

        /* A source with only Read, so that the reader needs a buffer too. */
        BytesReader br;
        bytes_reader_reset(&br, z);
        PlainReader pr = {bytes_reader_as_io_reader(&br)};
        LzwReader *r =
            lzw_new_reader(&al, (IoReader){&plain_reader_vt, &pr}, LZW_LSB, 8);
        if (r != NULL) {
            Error err;
            Slice got = io_read_all(scratch, lzw_reader_as_io_reader(r), &err);
            CHECK(BURROW_OK(err) && bytes_eq(got, in));
            lzw_reader_free(r);
            made++;
        }

        BytesBuffer out = BYTES_BUFFER(scratch);
        LzwWriter *w = lzw_new_writer(&al, bytes_buffer_as_io_writer(&out), LZW_LSB, 8);
        if (w != NULL) {
            Error err;
            lzw_writer_write(w, in, &err);
            CHECK(BURROW_OK(err) && BURROW_OK(lzw_writer_close(w)));
            CHECK(bytes_eq(bytes_buffer_bytes(&out), z));
            lzw_writer_free(w);
            made++;
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked", budget, (int)b.live);
    }
    CHECK(made > 0);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReader)                                                                      \
    X(TestReaderReset)                                                                 \
    X(TestHiCodeDoesNotOverflow)                                                       \
    X(TestNoLongerSavingPriorExpansions)                                               \
    X(TestWriter)                                                                      \
    X(TestWriterReset)                                                                 \
    X(TestWriterReturnValues)                                                          \
    X(TestSmallLitWidth)                                                               \
    X(TestStartsWithClearCode)                                                         \
    X(TestGoVectors)                                                                   \
    X(TestGoDecodeVectors)                                                             \
    X(TestBadOrderAndWidth)                                                            \
    X(TestClosed)                                                                      \
    X(TestWriterErrors)                                                                \
    X(TestWriterIntoBufio)                                                             \
    X(TestReaderStopsAtEnd)                                                            \
    X(TestInterfaces)                                                                  \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
