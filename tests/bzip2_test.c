/* Derived from Go's src/compress/bzip2/bzip2_test.go.
 * Go source: go1.27.1.
 *
 * TestReader and TestZeroRead are Go's. TestBitReader and TestMTF test
 * unexported helpers, which are static here, and every stream below goes
 * through both. Go's test files are in tests/bzip2_test_gen.h, from
 * tools/gen-bzip2-tests.sh, along with streams bzip2(1) made and what Go's
 * reader makes of each of them, whole and damaged in a few thousand ways. The
 * reader here has to give the same bytes and the same error every time.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/compress/bzip2.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "bzip2_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text_is(Str s, const char *want) {
    return (size_t)s.len == strlen(want) && memcmp(s.p, want, (size_t)s.len) == 0;
}

static bool bytes_eq(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static Slice must_decode_hex(Alloc *a, const char *s) {
    Error err;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        abort();
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
    if (BURROW_FAILED(err))
        abort();
    return out;
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

/* ----------------------------------------------------------- bzip2_test.go */

typedef struct ReaderVector {
    const char *desc;
    const char *input; /* hex */
    int gen;           /* or a stream from the generator, when input is NULL */
    int output;        /* 0 as given, 1 for 32 zeros, 2 for 1 MiB of zeros, 3
                           for 1 MiB of sawtooth */
    const char *output_hex;
    bool fail;
} ReaderVector;

static const ReaderVector reader_vectors[] = {
    {"hello world",
     "425a68393141592653594eece83600000251800010400006449080200031064c"
     "4101a7a9a580bb9431f8bb9229c28482776741b0",
     -1, 0, "68656c6c6f20776f726c640a", false},
    {"concatenated files",
     "425a68393141592653594eece83600000251800010400006449080200031064c"
     "4101a7a9a580bb9431f8bb9229c28482776741b0425a68393141592653594eec"
     "e83600000251800010400006449080200031064c4101a7a9a580bb9431f8bb92"
     "29c28482776741b0",
     -1, 0, "68656c6c6f20776f726c640a68656c6c6f20776f726c640a", false},
    {"32B zeros",
     "425a6839314159265359b5aa5098000000600040000004200021008283177245"
     "385090b5aa5098",
     -1, 1, NULL, false},
    {"1MiB zeros",
     "425a683931415926535938571ce50008084000c0040008200030cc0529a60806"
     "c4201e2ee48a70a12070ae39ca",
     -1, 2, NULL, false},
    /* "random data" and "random data - full symbol range" are Go's
     * pass-random1 and pass-random2, which TestGoStreams reads. */
    {"random data - uses RLE1 stage",
     "425a6839314159265359d992d0f60000137dfe84020310091c1e280e100e0428"
     "01099210094806c0110002e70806402000546034000034000000f28300000320"
     "00d3403264049270eb7a9280d308ca06ad28f6981bee1bf8160727c7364510d7"
     "3a1e123083421b63f031f63993a0f40051fbf177245385090d992d0f60",
     -1, 0,
     "92d5652616ac444a4a04af1a8a3964aca0450d43d6cf233bd03233f4ba92f871"
     "9e6c2a2bd4f5f88db07ecd0da3a33b263483db9b2c158786ad6363be35d17335"
     "ba",
     false},
    {"1MiB sawtooth", NULL, 2, 3, NULL, false},
    {"RLE2 buffer overrun - issue 5747", NULL, 3, 0, "", true},
    {"out-of-range selector - issue 8363",
     "425a68393141592653594eece83600000251800010400006449080200031064c"
     "4101a7a9a580bb943117724538509000000000",
     -1, 0, "", true},
    {"bad block size - issue 13941",
     "425a683131415926535936dc55330063ffc0006000200020a40830008b0008b8"
     "bb9229c28481b6e2a998",
     -1, 0, "", true},
    {"bad huffman delta",
     "425a6836314159265359b1f7404b000000400040002000217d184682ee48a70a"
     "12163ee80960",
     -1, 0, "", true},
};

static void TestReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof reader_vectors / sizeof reader_vectors[0]; i++) {
        const ReaderVector *v = &reader_vectors[i];
        Slice input = v->input != NULL ? must_decode_hex(a, v->input)
                                       : gen_bytes(a, bzip2_streams[v->gen].b64);
        Slice output;
        switch (v->output) {
        case 1:
            output = slice_from(mem_alloc(a, 32, 1), 32, 32, TYPE_BYTE);
            break;
        case 2:
            output = slice_from(mem_alloc(a, 1 << 20, 1), 1 << 20, 1 << 20, TYPE_BYTE);
            break;
        case 3:
            output = slice_from(mem_alloc(a, 1 << 20, 1), 1 << 20, 1 << 20, TYPE_BYTE);
            for (Int j = 0; j < output.len; j++)
                ((Byte *)output.p)[j] = (Byte)j;
            break;
        default:
            output = must_decode_hex(a, v->output_hex);
            break;
        }

        BytesReader br;
        bytes_reader_reset(&br, input);
        IoReader rd = bzip2_new_reader(a, bytes_reader_as_io_reader(&br));
        Error err;
        Slice buf = io_read_all(a, rd, &err);
        bool fail = BURROW_FAILED(err);
        if (fail != v->fail) {
            if (fail)
                testing_t_errorf_v(t, "test %d (%s), unexpected failure: %s", (int)i,
                                   v->desc, error_text(err));
            else
                testing_t_errorf_v(t, "test %d (%s), unexpected success", (int)i,
                                   v->desc);
        }
        if (!v->fail && !bytes_eq(buf, output))
            testing_t_errorf_v(t,
                               "test %d (%s), output mismatch: got %d bytes, want %d",
                               (int)i, v->desc, (int)buf.len, (int)output.len);
        bzip2_reader_free(rd);
    }
    arena_free(&ar);
}

static void TestZeroRead(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice b =
        must_decode_hex(a, "425a6839314159265359b5aa5098000000600040000004200021008"
                           "283177245385090b5aa5098");
    BytesReader br;
    bytes_reader_reset(&br, b);
    IoReader r = bzip2_new_reader(a, bytes_reader_as_io_reader(&br));
    Error err;
    Int n = BURROW_CALL(r, read, slice_nil(TYPE_BYTE), &err);
    if (n != 0 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Read(nil) = (%d, %s), want (0, nil)", (int)n,
                           error_text(err));
    bzip2_reader_free(r);
    arena_free(&ar);
}

/* ------------------------------------------------------------ burrow's own */

enum { SOURCE_BYTES_READER, SOURCE_PLAIN, SOURCE_BUFIO, SOURCE_COUNT };

typedef struct Result {
    Int len;
    char sha[65];
    Error err;
} Result;

/* Reads in 4096 byte reads to the first error, the way the generator does,
 * from a source of the given kind. */
static Result read_stream(Alloc *a, Slice in, int source, Int *left) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    PlainReader pr = {bytes_reader_as_io_reader(&br)};
    BufioReader *bio = NULL;
    IoReader src = pr.r;
    if (source == SOURCE_PLAIN) {
        src = (IoReader){&plain_reader_vt, &pr};
    } else if (source == SOURCE_BUFIO) {
        bio = bufio_new_reader(a, pr.r);
        src = bufio_reader_as_io_reader(bio);
    }
    IoReader r = bzip2_new_reader(a, src);
    if (r.vt == NULL)
        abort();
    BytesBuffer out = BYTES_BUFFER(a);
    Byte buf[4096];
    Result res = {0, {0}, BURROW_NO_ERROR};
    for (;;) {
        Error err;
        Int k = BURROW_CALL(r, read, slice_from(buf, 4096, 4096, TYPE_BYTE), &err);
        bytes_buffer_write(&out, slice_from(buf, k, k, TYPE_BYTE), NULL);
        res.len += k;
        if (BURROW_FAILED(err)) {
            if (!errors_is(err, io_eof))
                res.err = err;
            break;
        }
    }
    sha_hex(bytes_buffer_bytes(&out), res.sha);
    if (left != NULL)
        *left = bytes_reader_len(&br) + (bio != NULL ? bufio_reader_buffered(bio) : 0);
    bzip2_reader_free(r);
    bufio_reader_free(bio);
    return res;
}

static bool err_matches(Error err, const char *want) {
    if (want[0] == 0)
        return BURROW_OK(err);
    return BURROW_FAILED(err) && text_is(error_text(err), want);
}

/* Every stream the generator has, from each kind of source. */
static void TestGoStreams(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof bzip2_streams / sizeof bzip2_streams[0]; i++) {
        const Bzip2Stream *s = &bzip2_streams[i];
        Slice in = gen_bytes(a, s->b64);
        for (int source = 0; source < SOURCE_COUNT; source++) {
            Int left;
            Result r = read_stream(a, in, source, &left);
            if (r.len != s->len || strcmp(r.sha, s->sha256) != 0 ||
                !err_matches(r.err, s->err))
                testing_t_errorf_v(t,
                                   "%s, source %d: %d bytes and \"%s\", want %d and "
                                   "\"%s\" as Go",
                                   s->name, source, (int)r.len, error_text(r.err),
                                   (int)s->len, s->err);
            /* A reader with ReadByte is read to the end of the stream and no
             * further, which here is the end of the input. */
            if (s->err[0] == 0 && left != 0)
                testing_t_errorf_v(t, "%s, source %d: %d bytes left unread", s->name,
                                   source, (int)left);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* The small streams with bits flipped, cut short and with more after them. */
static void TestGoDamage(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    enum { NSTREAMS = sizeof bzip2_streams / sizeof bzip2_streams[0] };
    Slice streams[NSTREAMS];
    for (int i = 0; i < NSTREAMS; i++)
        streams[i] = gen_bytes(a, bzip2_streams[i].b64);
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    for (size_t i = 0; i < sizeof bzip2_damage / sizeof bzip2_damage[0]; i++) {
        const Bzip2Damage *d = &bzip2_damage[i];
        Slice base = streams[d->stream];
        Slice in;
        if (d->kind == 0) {
            in = slice_from(mem_alloc(sa, (size_t)base.len, 1), base.len, base.len,
                            TYPE_BYTE);
            memcpy(in.p, base.p, (size_t)base.len);
            ((Byte *)in.p)[d->pos] ^= (Byte)d->mask;
        } else if (d->kind == 1) {
            in = slice_from(base.p, d->pos, d->pos, TYPE_BYTE);
        } else {
            const char *suf = bzip2_suffixes[d->pos];
            Int n = base.len + (Int)strlen(suf);
            in = slice_from(mem_alloc(sa, (size_t)n, 1), n, n, TYPE_BYTE);
            memcpy(in.p, base.p, (size_t)base.len);
            memcpy((Byte *)in.p + base.len, suf, strlen(suf));
        }
        for (int source = 0; source <= SOURCE_PLAIN; source++) {
            Result r = read_stream(sa, in, source, NULL);
            if (r.len != d->len || strncmp(r.sha, d->sha256, 16) != 0 ||
                !err_matches(r.err, d->err))
                testing_t_errorf_v(t,
                                   "damage %d (stream %d kind %d pos %d mask %d), "
                                   "source %d: %d bytes and \"%s\", want %d and "
                                   "\"%s\" as Go",
                                   (int)i, d->stream, d->kind, d->pos, d->mask, source,
                                   (int)r.len, error_text(r.err), (int)d->len, d->err);
        }
        arena_reset(&scratch);
    }
    arena_free(&scratch);
    arena_free(&ar);
}

/* After the end every read says so again. After an error the next read gives
 * whatever Go's does. */
static void TestReadAfterEnd(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = must_decode_hex(a, reader_vectors[0].input);
    BytesReader br;
    bytes_reader_reset(&br, in);
    IoReader r = bzip2_new_reader(a, bytes_reader_as_io_reader(&br));
    Error err;
    Slice text = io_read_all(a, r, &err);
    CHECK(BURROW_OK(err) && text.len == 12);
    Byte buf[16];
    for (int i = 0; i < 3; i++) {
        CHECK(BURROW_CALL(r, read, slice_from(buf, 16, 16, TYPE_BYTE), &err) == 0);
        CHECK(errors_is(err, io_eof));
    }
    bzip2_reader_free(r);

    bytes_reader_reset(&br, slice_from(in.p, 20, 20, TYPE_BYTE));
    r = bzip2_new_reader(a, bytes_reader_as_io_reader(&br));
    io_read_all(a, r, &err);
    CHECK(errors_is(err, io_err_unexpected_eof));
    /* Go's reader does not keep the error. The next read finds the block it
     * gave up on unfinished and says its checksum is wrong, and so does this
     * one. */
    CHECK(BURROW_CALL(r, read, slice_from(buf, 16, 16, TYPE_BYTE), &err) == 0);
    CHECK(text_is(error_text(err), "bzip2 data invalid: block checksum mismatch"));
    bzip2_reader_free(r);
    arena_free(&ar);
}

static void TestStructuralError(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br, slice_from((void *)(uintptr_t)"BZh0", 4, 4, TYPE_BYTE));
    IoReader r = bzip2_new_reader(a, bytes_reader_as_io_reader(&br));
    Byte buf[4];
    Error err;
    CHECK(BURROW_CALL(r, read, slice_from(buf, 4, 4, TYPE_BYTE), &err) == 0);
    CHECK(text_is(error_text(err), "bzip2 data invalid: invalid compression level"));
    Error want =
        bzip2_structural_error_as_error(BURROW_S("invalid compression level"), a);
    CHECK(errors_is(err, want));
    CHECK(!errors_is(err,
                     bzip2_structural_error_as_error(BURROW_S("bad magic value"), a)));
    const Bzip2StructuralError *se =
        (const Bzip2StructuralError *)errors_as(err, TYPE_BZIP2_STRUCTURAL_ERROR);
    CHECK(se != NULL && text_is(*se, "invalid compression level"));
    CHECK(text_is(bzip2_structural_error_error(BURROW_S("x"), a),
                  "bzip2 data invalid: x"));
    Error c = error_retain(a, err);
    CHECK(errors_is(c, err) && errors_as(c, TYPE_BZIP2_STRUCTURAL_ERROR) != NULL);
    bzip2_reader_free(r);
    arena_free(&ar);
}

static void TestFree(TestingT *t) {
    (void)t;
    bzip2_reader_free((IoReader){NULL, NULL});
    /* Not a bzip2 reader, so left alone. */
    BytesReader br;
    bytes_reader_reset(&br, slice_nil(TYPE_BYTE));
    bzip2_reader_free(bytes_reader_as_io_reader(&br));
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
    /* Two files, the second with bigger blocks, so the block buffer is made
     * twice. */
    const Bzip2Stream *s = &bzip2_streams[11];
    Slice in = gen_bytes(scratch, s->b64);
    int made = 0, read = 0;
    for (int budget = 0; budget < 6; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        BytesReader br;
        bytes_reader_reset(&br, in);
        PlainReader pr = {bytes_reader_as_io_reader(&br)};
        IoReader r = bzip2_new_reader(&al, (IoReader){&plain_reader_vt, &pr});
        if (r.vt != NULL) {
            made++;
            Error err;
            Slice got = io_read_all(scratch, r, &err);
            if (BURROW_OK(err)) {
                char hex[65];
                sha_hex(got, hex);
                CHECK(strcmp(hex, s->sha256) == 0);
                read++;
            } else {
                CHECK(errors_is(err, burrow_err_out_of_memory));
            }
            bzip2_reader_free(r);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked", budget, (int)b.live);
    }
    CHECK(made > read && read > 0);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReader)                                                                      \
    X(TestZeroRead)                                                                    \
    X(TestGoStreams)                                                                   \
    X(TestGoDamage)                                                                    \
    X(TestReadAfterEnd)                                                                \
    X(TestStructuralError)                                                             \
    X(TestFree)                                                                        \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
