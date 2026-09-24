/* Derived from Go's src/io/io_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* Errors the tests fail with, made the way Go's errors.New would be but with
 * nothing to free. */
#define TEST_ERROR(name, text)                                                         \
    static const Str name##__text = {(const Byte *)(text), (Int)(sizeof(text) - 1)};   \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

TEST_ERROR(read_error, "readError");
TEST_ERROR(write_error, "writeError");
TEST_ERROR(fake_error, "fake error");
TEST_ERROR(large_writer_error, "largeWriterError");
TEST_ERROR(out_of_range, "memFile: write out of range");
TEST_ERROR(wanted_and_err, "wantedAndErrReader error");

static Alloc *heap(void) {
    return heap_allocator();
}

static bool holds(BytesBuffer *b, const char *want) {
    Slice got = bytes_buffer_bytes(b);
    return str_eq(str_from_bytes((const Byte *)got.p, got.len), str_from_cstr(want));
}

/* ------------------------------------------------------------------ Buffer
 *
 * Go's test Buffer embeds a bytes.Buffer and hides its ReadFrom and WriteTo, so
 * io_copy has to take the slow path. Here that is a wrapper whose descriptor
 * lists no methods at all. */
typedef struct Buffer {
    BytesBuffer b;
} Buffer;

static const Type buffer_type = {
    {(const Byte *)"Buffer", 6},
    {(const Byte *)"io_test", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(Buffer),
    (uint16_t)_Alignof(Buffer),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x62756666U,
    NULL,
};

static Int buffer_read(void *self, Slice p, Error *err) {
    return bytes_buffer_read(&((Buffer *)self)->b, p, err);
}

static Int buffer_write(void *self, Slice p, Error *err) {
    return bytes_buffer_write(&((Buffer *)self)->b, p, err);
}

static const IoReaderVT buffer_reader_vt = {&buffer_type, buffer_read};
static const IoWriterVT buffer_writer_vt = {&buffer_type, buffer_write};

static IoReader buffer_as_io_reader(Buffer *b) {
    IoReader r = {&buffer_reader_vt, b};
    return r;
}

static IoWriter buffer_as_io_writer(Buffer *b) {
    IoWriter w = {&buffer_writer_vt, b};
    return w;
}

static Buffer new_buffer(void) {
    Buffer b = {BYTES_BUFFER(heap())};
    return b;
}

static void TestCopy(TestingT *t) {
    Buffer rb = new_buffer(), wb = new_buffer();
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    io_copy(heap(), buffer_as_io_writer(&wb), buffer_as_io_reader(&rb), NULL);
    CHECK(holds(&wb.b, "hello, world."));
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb.b);
}

static void TestCopyNegative(TestingT *t) {
    Buffer rb = new_buffer(), wb = new_buffer();
    bytes_buffer_write_string(&rb.b, S("hello"), NULL);
    IoLimitedReader l = io_limit_reader(buffer_as_io_reader(&rb), -1);
    io_copy(heap(), buffer_as_io_writer(&wb), io_limited_reader_as_io_reader(&l), NULL);
    CHECK(holds(&wb.b, ""));

    io_copy_n(heap(), buffer_as_io_writer(&wb), buffer_as_io_reader(&rb), -1, NULL);
    CHECK(holds(&wb.b, ""));
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb.b);
}

static void TestCopyBuffer(TestingT *t) {
    Buffer rb = new_buffer(), wb = new_buffer();
    Byte one[1];
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    /* Tiny buffer to keep it honest. */
    io_copy_buffer(buffer_as_io_writer(&wb), buffer_as_io_reader(&rb),
                   slice_from(one, 1, 1, TYPE_BYTE), NULL);
    CHECK(holds(&wb.b, "hello, world."));
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb.b);
}

/* Go's CopyBuffer allocates when handed nil. There is nothing to allocate from
 * here, so a nil buffer is io_err_short_buffer and copies nothing, and io_copy
 * is the call that allocates. */
static void TestCopyBufferNil(TestingT *t) {
    Buffer rb = new_buffer(), wb = new_buffer();
    Error err = BURROW_NO_ERROR;
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    int64_t n = io_copy_buffer(buffer_as_io_writer(&wb), buffer_as_io_reader(&rb),
                               slice_nil(TYPE_BYTE), &err);
    CHECK(n == 0);
    CHECK(errors_is(err, io_err_short_buffer));
    CHECK(holds(&wb.b, ""));
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb.b);
}

static void TestCopyReadFrom(TestingT *t) {
    Buffer rb = new_buffer();
    BytesBuffer wb = BYTES_BUFFER(heap()); /* implements ReadFrom. */
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    io_copy(heap(), bytes_buffer_as_io_writer(&wb), buffer_as_io_reader(&rb), NULL);
    CHECK(holds(&wb, "hello, world."));
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb);
}

static void TestCopyWriteTo(TestingT *t) {
    BytesBuffer rb = BYTES_BUFFER(heap()); /* implements WriteTo. */
    Buffer wb = new_buffer();
    bytes_buffer_write_string(&rb, S("hello, world."), NULL);
    io_copy(heap(), buffer_as_io_writer(&wb), bytes_buffer_as_io_reader(&rb), NULL);
    CHECK(holds(&wb.b, "hello, world."));
    bytes_buffer_free(&rb);
    bytes_buffer_free(&wb.b);
}

/* ---------------------------------------------------------- writeToChecker */

typedef struct WriteToChecker {
    BytesBuffer b;
    bool write_to_called;
} WriteToChecker;

static int64_t write_to_checker_write_to(WriteToChecker *wt, IoWriter w, Error *err) {
    wt->write_to_called = true;
    return bytes_buffer_write_to(&wt->b, w, err);
}

#define WRITE_TO_CHECKER_METHODS(M, T)                                                 \
    M(T, WriteTo, write_to_checker_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(WriteToChecker, WRITE_TO_CHECKER_METHODS);

static const Type write_to_checker_type = {
    {(const Byte *)"writeToChecker", 14},
    {(const Byte *)"io_test", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(WriteToChecker),
    (uint16_t)_Alignof(WriteToChecker),
    0,
    (uint16_t)(sizeof burrow__methods_WriteToChecker /
               sizeof burrow__methods_WriteToChecker[0]),
    NULL,
    burrow__methods_WriteToChecker,
    NULL,
    NULL,
    0,
    0x77746368U,
    NULL,
};

static Int write_to_checker_read(void *self, Slice p, Error *err) {
    return bytes_buffer_read(&((WriteToChecker *)self)->b, p, err);
}

static const IoReaderVT write_to_checker_reader_vt = {&write_to_checker_type,
                                                      write_to_checker_read};

static void TestCopyPriority(TestingT *t) {
    WriteToChecker rb = {BYTES_BUFFER(heap()), false};
    BytesBuffer wb = BYTES_BUFFER(heap());
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    IoReader r = {&write_to_checker_reader_vt, &rb};
    io_copy(heap(), bytes_buffer_as_io_writer(&wb), r, NULL);
    CHECK(holds(&wb, "hello, world."));
    /* WriteTo was prioritized over ReadFrom. */
    CHECK(rb.write_to_called);
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb);
}

/* --------------------------------------------- zeroErrReader and errWriter */

typedef struct ErrHolder {
    Error err;
} ErrHolder;

static const Type err_holder_type = {
    {(const Byte *)"errHolder", 9},
    {(const Byte *)"io_test", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(ErrHolder),
    (uint16_t)_Alignof(ErrHolder),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65727268U,
    NULL,
};

static Int zero_err_read(void *self, Slice p, Error *err) {
    Int n = 0;
    if (p.len > 0) {
        *(Byte *)p.p = 0;
        n = 1;
    }
    *err = ((ErrHolder *)self)->err;
    return n;
}

static Int err_write(void *self, Slice p, Error *err) {
    (void)p;
    *err = ((ErrHolder *)self)->err;
    return 0;
}

/* largeWriter says it wrote one byte more than it was given. */
static Int large_write(void *self, Slice p, Error *err) {
    *err = ((ErrHolder *)self)->err;
    return p.len + 1;
}

static const IoReaderVT zero_err_reader_vt = {&err_holder_type, zero_err_read};
static const IoWriterVT err_writer_vt = {&err_holder_type, err_write};
static const IoWriterVT large_writer_vt = {&err_holder_type, large_write};

static void TestCopyReadErrWriteErr(TestingT *t) {
    ErrHolder er = {read_error};
    ErrHolder ew = {write_error};
    IoReader r = {&zero_err_reader_vt, &er};
    IoWriter w = {&err_writer_vt, &ew};
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(heap(), w, r, &err);
    CHECK(n == 0);
    CHECK(errors_is(err, ew.err));
}

static void TestCopyN(TestingT *t) {
    Buffer rb = new_buffer(), wb = new_buffer();
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    io_copy_n(heap(), buffer_as_io_writer(&wb), buffer_as_io_reader(&rb), 5, NULL);
    CHECK(holds(&wb.b, "hello"));
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb.b);
}

static void TestCopyNReadFrom(TestingT *t) {
    Buffer rb = new_buffer();
    BytesBuffer wb = BYTES_BUFFER(heap()); /* implements ReadFrom. */
    bytes_buffer_write_string(&rb.b, S("hello"), NULL);
    io_copy_n(heap(), bytes_buffer_as_io_writer(&wb), buffer_as_io_reader(&rb), 5,
              NULL);
    CHECK(holds(&wb, "hello"));
    bytes_buffer_free(&rb.b);
    bytes_buffer_free(&wb);
}

static void TestCopyNWriteTo(TestingT *t) {
    BytesBuffer rb = BYTES_BUFFER(heap()); /* implements WriteTo. */
    Buffer wb = new_buffer();
    bytes_buffer_write_string(&rb, S("hello, world."), NULL);
    io_copy_n(heap(), buffer_as_io_writer(&wb), bytes_buffer_as_io_reader(&rb), 5,
              NULL);
    CHECK(holds(&wb.b, "hello"));
    bytes_buffer_free(&rb);
    bytes_buffer_free(&wb.b);
}

/* noReadFrom hides a writer's ReadFrom behind a type with no methods. */
static Int no_read_from_write(void *self, Slice p, Error *err) {
    return bytes_buffer_write((BytesBuffer *)self, p, err);
}

static const IoWriterVT no_read_from_vt = {&buffer_type, no_read_from_write};

static Int wanted_and_err_read(void *self, Slice p, Error *err) {
    (void)self;
    *err = wanted_and_err;
    return p.len;
}

static const IoReaderVT wanted_and_err_vt = {&err_holder_type, wanted_and_err_read};

static void TestCopyNEOF(TestingT *t) {
    BytesBuffer b = BYTES_BUFFER(heap());
    IoWriter nrf = {&no_read_from_vt, &b};
    IoReader wae = {&wanted_and_err_vt, NULL};
    StringsReader sr;
    Error err;
    int64_t n;

    /* Test that EOF behavior is the same regardless of whether argument 1
     * to CopyN has a ReadFrom. */
    strings_reader_reset(&sr, S("foo"));
    err = BURROW_NO_ERROR;
    n = io_copy_n(heap(), nrf, strings_reader_as_io_reader(&sr), 3, &err);
    CHECK(n == 3 && BURROW_OK(err));

    strings_reader_reset(&sr, S("foo"));
    err = BURROW_NO_ERROR;
    n = io_copy_n(heap(), nrf, strings_reader_as_io_reader(&sr), 4, &err);
    CHECK(n == 3 && errors_is(err, io_eof));

    strings_reader_reset(&sr, S("foo"));
    err = BURROW_NO_ERROR;
    n = io_copy_n(heap(), bytes_buffer_as_io_writer(&b),
                  strings_reader_as_io_reader(&sr), 3, &err);
    CHECK(n == 3 && BURROW_OK(err));

    strings_reader_reset(&sr, S("foo"));
    err = BURROW_NO_ERROR;
    n = io_copy_n(heap(), bytes_buffer_as_io_writer(&b),
                  strings_reader_as_io_reader(&sr), 4, &err);
    CHECK(n == 3 && errors_is(err, io_eof));

    err = BURROW_NO_ERROR;
    n = io_copy_n(heap(), bytes_buffer_as_io_writer(&b), wae, 5, &err);
    CHECK(n == 5 && BURROW_OK(err));

    err = BURROW_NO_ERROR;
    n = io_copy_n(heap(), nrf, wae, 5, &err);
    CHECK(n == 5 && BURROW_OK(err));
    bytes_buffer_free(&b);
}

/* ------------------------------------------------------------ ReadAtLeast */

/* dataAndErrorBuffer returns err along with the last bytes. A zero err is a
 * plain bytes.Buffer. */
typedef struct DataAndErrorBuffer {
    Error err;
    BytesBuffer b;
} DataAndErrorBuffer;

static Int data_and_error_read(void *self, Slice p, Error *err) {
    DataAndErrorBuffer *r = (DataAndErrorBuffer *)self;
    Error e = BURROW_NO_ERROR;
    Int n = bytes_buffer_read(&r->b, p, &e);
    if (n > 0 && bytes_buffer_len(&r->b) == 0 && BURROW_OK(e))
        e = r->err;
    if (BURROW_FAILED(e))
        *err = e;
    return n;
}

static const IoReaderVT data_and_error_vt = {&err_holder_type, data_and_error_read};

static void test_read_at_least(TestingT *t, DataAndErrorBuffer *rb) {
    IoReader r = {&data_and_error_vt, rb};
    Byte b2[2];
    Slice buf = slice_from(b2, 2, 2, TYPE_BYTE);
    Error err;
    Int n;

    bytes_buffer_write_string(&rb->b, S("0123"), NULL);
    err = BURROW_NO_ERROR;
    n = io_read_at_least(r, buf, 2, &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(n, 2);

    err = BURROW_NO_ERROR;
    n = io_read_at_least(r, buf, 4, &err);
    CHECK(errors_is(err, io_err_short_buffer));
    CHECK_INT_EQ(n, 0);

    err = BURROW_NO_ERROR;
    n = io_read_at_least(r, buf, 1, &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(n, 2);

    err = BURROW_NO_ERROR;
    n = io_read_at_least(r, buf, 2, &err);
    CHECK(errors_is(err, io_eof));
    CHECK_INT_EQ(n, 0);

    bytes_buffer_write_string(&rb->b, S("4"), NULL);
    err = BURROW_NO_ERROR;
    n = io_read_at_least(r, buf, 2, &err);
    Error want = io_err_unexpected_eof;
    if (BURROW_FAILED(rb->err) && !errors_is(rb->err, io_eof))
        want = rb->err;
    CHECK(errors_is(err, want));
    CHECK_INT_EQ(n, 1);
}

static void TestReadAtLeast(TestingT *t) {
    DataAndErrorBuffer rb = {BURROW_NO_ERROR, BYTES_BUFFER(heap())};
    test_read_at_least(t, &rb);
    bytes_buffer_free(&rb.b);
}

static void TestReadAtLeastWithDataAndEOF(TestingT *t) {
    DataAndErrorBuffer rb = {io_eof, BYTES_BUFFER(heap())};
    test_read_at_least(t, &rb);
    bytes_buffer_free(&rb.b);
}

static void TestReadAtLeastWithDataAndError(TestingT *t) {
    DataAndErrorBuffer rb = {fake_error, BYTES_BUFFER(heap())};
    test_read_at_least(t, &rb);
    bytes_buffer_free(&rb.b);
}

/* -------------------------------------------------------------- TeeReader */

static void TestTeeReader(TestingT *t) {
    static const char src[] = "hello, world";
    Int len = (Int)(sizeof src - 1);
    Byte dst[sizeof src - 1];
    Slice dsts = slice_from(dst, len, len, TYPE_BYTE);
    BytesReader rb;
    BytesBuffer wb = BYTES_BUFFER(heap());
    Error err = BURROW_NO_ERROR;

    bytes_reader_reset(&rb, BURROW_B("hello, world"));
    IoTeeReader tr =
        io_tee_reader(bytes_reader_as_io_reader(&rb), bytes_buffer_as_io_writer(&wb));
    IoReader r = io_tee_reader_as_io_reader(&tr);
    Int n = io_read_full(r, dsts, &err);
    CHECK(BURROW_OK(err) && n == len);
    CHECK(memcmp(dst, src, (size_t)len) == 0);
    CHECK(holds(&wb, src));

    err = BURROW_NO_ERROR;
    n = r.vt->read(r.data, dsts, &err);
    CHECK(n == 0 && errors_is(err, io_eof));

    bytes_reader_reset(&rb, BURROW_B("hello, world"));
    IoPipeReader *pr;
    IoPipeWriter *pw;
    io_pipe(heap(), &pr, &pw);
    io_pipe_reader_close(pr);
    tr = io_tee_reader(bytes_reader_as_io_reader(&rb), io_pipe_writer_as_io_writer(pw));
    err = BURROW_NO_ERROR;
    n = io_read_full(io_tee_reader_as_io_reader(&tr), dsts, &err);
    CHECK(n == 0 && errors_is(err, io_err_closed_pipe));
    io_pipe_free(pr);
    bytes_buffer_free(&wb);
}

/* ---------------------------------------------------------- SectionReader */

static void TestSectionReader_ReadAt(TestingT *t) {
    static const char dat[] = "a long sample data, 1234567890";
    const Int d = (Int)(sizeof dat - 1);
    const struct {
        const char *data;
        Int off, n, buf_len, at;
        Int exp_from, exp_len;
        bool eof;
    } tests[] = {
        {"", 0, 10, 2, 0, 0, 0, true},
        {dat, 0, d, 0, 0, 0, 0, false},
        {dat, d, 1, 1, 0, 0, 0, true},
        {dat, 0, d + 2, d, 0, 0, d, false},
        {dat, 0, d, d / 2, 0, 0, d / 2, false},
        {dat, 0, d, d, 0, 0, d, false},
        {dat, 0, d, d / 2, 2, 2, d / 2, false},
        {dat, 3, d, d / 2, 2, 5, d / 2, false},
        {dat, 3, d / 2, d / 2 - 2, 2, 5, d / 2 - 2, false},
        {dat, 3, d / 2, d / 2 + 2, 2, 5, d / 2 - 2, true},
        {dat, 0, 0, 0, -1, 0, 0, true},
        {dat, 0, 0, 0, 1, 0, 0, true},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        StringsReader r;
        Byte buf[64];
        Error err = BURROW_NO_ERROR;
        strings_reader_reset(&r, str_from_cstr(tests[i].data));
        IoReaderAt ra = strings_reader_as_io_reader_at(&r);
        IoSectionReader s = io_new_section_reader(ra, tests[i].off, tests[i].n);
        Int n = io_section_reader_read_at(
            &s, slice_from(buf, tests[i].buf_len, tests[i].buf_len, TYPE_BYTE),
            tests[i].at, &err);
        CHECK_INT_EQ(n, tests[i].exp_len);
        CHECK(memcmp(buf, dat + tests[i].exp_from, (size_t)n) == 0);
        CHECK(tests[i].eof ? errors_is(err, io_eof) : BURROW_OK(err));

        int64_t off, sn;
        IoReaderAt outer = io_section_reader_outer(&s, &off, &sn);
        CHECK(outer.vt == ra.vt && outer.data == ra.data);
        CHECK(off == tests[i].off && sn == tests[i].n);
    }
}

static void TestSectionReader_Seek(TestingT *t) {
    /* Verifies that NewSectionReader's Seeker behaves like bytes.NewReader
     * (which is like strings.NewReader). */
    BytesReader br;
    bytes_reader_reset(&br, BURROW_B("foo"));
    IoSectionReader sr = io_new_section_reader(bytes_reader_as_io_reader_at(&br), 0, 3);

    static const int whences[] = {BURROW_IO_SEEK_START, BURROW_IO_SEEK_CURRENT,
                                  BURROW_IO_SEEK_END};
    for (Int w = 0; w < LEN(whences); w++) {
        for (int64_t offset = -3; offset <= 4; offset++) {
            Error br_err = BURROW_NO_ERROR, sr_err = BURROW_NO_ERROR;
            int64_t br_off = bytes_reader_seek(&br, offset, whences[w], &br_err);
            int64_t sr_off = io_section_reader_seek(&sr, offset, whences[w], &sr_err);
            if (BURROW_FAILED(br_err) != BURROW_FAILED(sr_err) || br_off != sr_off)
                testing_t_errorf_v(
                    t,
                    "For whence %d, offset %d: bytes.Reader.Seek = %d != "
                    "SectionReader.Seek = %d",
                    whences[w], offset, br_off, sr_off);
        }
    }

    /* And verify we can just seek past the end and get an EOF. */
    Error err = BURROW_NO_ERROR;
    int64_t got = io_section_reader_seek(&sr, 100, BURROW_IO_SEEK_START, &err);
    CHECK(BURROW_OK(err) && got == 100);

    Byte buf[10];
    err = BURROW_NO_ERROR;
    Int n = io_section_reader_read(&sr, slice_from(buf, 10, 10, TYPE_BYTE), &err);
    CHECK(n == 0 && errors_is(err, io_eof));
}

static void TestSectionReader_Size(TestingT *t) {
    const struct {
        const char *data;
        int64_t want;
    } tests[] = {
        {"a long sample data, 1234567890", 30},
        {"", 0},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        StringsReader r;
        Str data = str_from_cstr(tests[i].data);
        strings_reader_reset(&r, data);
        IoSectionReader sr =
            io_new_section_reader(strings_reader_as_io_reader_at(&r), 0, data.len);
        CHECK(io_section_reader_size(&sr) == tests[i].want);
    }
}

static void TestSectionReader_Max(TestingT *t) {
    StringsReader r;
    strings_reader_reset(&r, S("abcdef"));
    IoReaderAt ra = strings_reader_as_io_reader_at(&r);
    IoSectionReader sr = io_new_section_reader(ra, 3, INT64_MAX);
    Byte buf[3];
    Error err = BURROW_NO_ERROR;
    Int n = io_section_reader_read(&sr, slice_from(buf, 3, 3, TYPE_BYTE), &err);
    CHECK(n == 3 && BURROW_OK(err));
    n = io_section_reader_read(&sr, slice_from(buf, 3, 3, TYPE_BYTE), &err);
    CHECK(n == 0 && errors_is(err, io_eof));

    int64_t off, sn;
    IoReaderAt outer = io_section_reader_outer(&sr, &off, &sn);
    CHECK(outer.vt == ra.vt && outer.data == ra.data);
    CHECK(off == 3 && sn == INT64_MAX);
}

/* ---------------------------------------------------------- largeWriter */

static void TestCopyLargeWriter(TestingT *t) {
    Buffer rb = new_buffer();
    ErrHolder h = {BURROW_NO_ERROR};
    IoWriter wb = {&large_writer_vt, &h};
    Error err = BURROW_NO_ERROR;
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    io_copy(heap(), wb, buffer_as_io_reader(&rb), &err);
    CHECK(str_eq(error_text(err), S("invalid write result")));
    bytes_buffer_free(&rb.b);

    h.err = large_writer_error;
    rb = new_buffer();
    bytes_buffer_write_string(&rb.b, S("hello, world."), NULL);
    err = BURROW_NO_ERROR;
    io_copy(heap(), wb, buffer_as_io_reader(&rb), &err);
    CHECK(errors_is(err, h.err));
    bytes_buffer_free(&rb.b);
}

/* -------------------------------------------------------------- NopCloser */

static bool has_write_to(const Type *t) {
    return t != NULL && type_method_by_name(t, S("WriteTo")) != NULL;
}

static void TestNopCloserWriterToForwarding(TestingT *t) {
    /* not a WriterTo */
    IoNopCloser nc = io_nop_closer((IoReader){NULL, NULL});
    IoReadCloser rc = io_nop_closer_as_io_read_closer(&nc);
    CHECK(!has_write_to(rc.vt->reader.self_type));

    /* a WriterTo */
    WriteToChecker wt = {BYTES_BUFFER(heap()), false};
    IoReader r = {&write_to_checker_reader_vt, &wt};
    CHECK(has_write_to(r.vt->self_type));
    nc = io_nop_closer(r);
    rc = io_nop_closer_as_io_read_closer(&nc);
    CHECK(has_write_to(rc.vt->reader.self_type));
}

/* ------------------------------------------------------------ OffsetWriter
 *
 * Go runs these against temporary files. A file here is a byte array that
 * knows its length, which is all WriteAt and ReadAt ask of one. */
typedef struct MemFile {
    SyncMutex mu;
    Byte buf[64];
    Int len;
} MemFile;

static const Type mem_file_type = {
    {(const Byte *)"memFile", 7},
    {(const Byte *)"io_test", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(MemFile),
    (uint16_t)_Alignof(MemFile),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6d656d66U,
    NULL,
};

static Int mem_file_write_at(void *self, Slice p, int64_t off, Error *err) {
    MemFile *f = (MemFile *)self;
    if (off < 0 || off + p.len > (int64_t)sizeof f->buf) {
        *err = out_of_range;
        return 0;
    }
    sync_mutex_lock(&f->mu);
    memcpy(f->buf + off, p.p, (size_t)p.len);
    if ((Int)off + p.len > f->len)
        f->len = (Int)off + p.len;
    sync_mutex_unlock(&f->mu);
    return p.len;
}

static Int mem_file_read_at(void *self, Slice p, int64_t off, Error *err) {
    MemFile *f = (MemFile *)self;
    Int n = 0;
    sync_mutex_lock(&f->mu);
    if (off < f->len) {
        n = f->len - (Int)off;
        if (n > p.len)
            n = p.len;
        memcpy(p.p, f->buf + off, (size_t)n);
    }
    sync_mutex_unlock(&f->mu);
    if (n < p.len)
        *err = io_eof;
    return n;
}

static const IoWriterAtVT mem_file_writer_at_vt = {&mem_file_type, mem_file_write_at};
static const IoReaderAtVT mem_file_reader_at_vt = {&mem_file_type, mem_file_read_at};

static IoWriterAt mem_file_writer_at(MemFile *f) {
    IoWriterAt w = {&mem_file_writer_at_vt, f};
    return w;
}

static IoReaderAt mem_file_reader_at(MemFile *f) {
    IoReaderAt r = {&mem_file_reader_at_vt, f};
    return r;
}

static void TestOffsetWriter_Seek(TestingT *t) {
    static MemFile f;
    IoOffsetWriter w = io_new_offset_writer(mem_file_writer_at(&f), 0);

    /* errWhence */
    static const int bad[] = {-3, -2, -1, 3, 4, 5};
    for (Int i = 0; i < LEN(bad); i++) {
        Error err = BURROW_NO_ERROR;
        int64_t off = io_offset_writer_seek(&w, 0, bad[i], &err);
        CHECK(off == 0 && str_eq(error_text(err), S("Seek: invalid whence")));
    }

    /* errOffset */
    static const int good[] = {BURROW_IO_SEEK_START, BURROW_IO_SEEK_CURRENT};
    for (Int i = 0; i < LEN(good); i++) {
        for (int64_t offset = -3; offset < 0; offset++) {
            Error err = BURROW_NO_ERROR;
            int64_t off = io_offset_writer_seek(&w, offset, good[i], &err);
            CHECK(off == 0 && str_eq(error_text(err), S("Seek: invalid offset")));
        }
    }

    /* normal */
    const struct {
        int64_t offset;
        int whence;
        int64_t return_off;
    } tests[] = {
        /* keep in order */
        {1, BURROW_IO_SEEK_START, 1},   {2, BURROW_IO_SEEK_START, 2},
        {3, BURROW_IO_SEEK_START, 3},   {1, BURROW_IO_SEEK_CURRENT, 4},
        {2, BURROW_IO_SEEK_CURRENT, 6}, {3, BURROW_IO_SEEK_CURRENT, 9},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        Error err = BURROW_NO_ERROR;
        int64_t off = io_offset_writer_seek(&w, tests[i].offset, tests[i].whence, &err);
        CHECK(off == tests[i].return_off && BURROW_OK(err));
    }
}

static const char content[] = "0123456789ABCDEF";
#define CONTENT_SIZE ((Int)(sizeof content - 1))

typedef struct WriteAtJob {
    MemFile *f;
    Byte value;
    int64_t off, at;
    Int step;
    SyncAtomicInt64 *write_n;
} WriteAtJob;

static void write_at_job(void *env) {
    WriteAtJob *j = (WriteAtJob *)env;
    IoOffsetWriter w = io_new_offset_writer(mem_file_writer_at(j->f), j->off);
    Error e = BURROW_NO_ERROR;
    Int n = io_offset_writer_write_at(&w, slice_from(&j->value, 1, 1, TYPE_BYTE),
                                      j->at + j->step, &e);
    if (BURROW_OK(e))
        sync_atomic_int64_add(j->write_n, n);
}

static void TestOffsetWriter_WriteAt(TestingT *t) {
    for (int64_t off = 0; off < 2; off++) {
        for (int64_t at = 0; at < 2; at++) {
            static MemFile f;
            memset(&f, 0, sizeof f);
            SyncAtomicInt64 write_n = {0};
            SyncWaitGroup wg = {0};
            WriteAtJob jobs[CONTENT_SIZE];
            for (Int step = 0; step < CONTENT_SIZE; step++) {
                jobs[step] =
                    (WriteAtJob){&f, (Byte)content[step], off, at, step, &write_n};
                sync_wait_group_go(&wg, BURROW_FN(Func, write_at_job, &jobs[step]));
            }
            sync_wait_group_wait(&wg);

            Byte buf[CONTENT_SIZE + 1];
            Error err = BURROW_NO_ERROR;
            Int read_n = mem_file_read_at(
                &f, slice_from(buf, CONTENT_SIZE + 1, CONTENT_SIZE + 1, TYPE_BYTE),
                off + at, &err);
            CHECK(errors_is(err, io_eof));
            CHECK(sync_atomic_int64_load(&write_n) == read_n);
            CHECK_INT_EQ(read_n, CONTENT_SIZE);
            CHECK(memcmp(buf, content, (size_t)CONTENT_SIZE) == 0);
        }
    }
}

static void TestWriteAt_PositionPriorToBase(TestingT *t) {
    static MemFile f;
    IoOffsetWriter w = io_new_offset_writer(mem_file_writer_at(&f), 10);
    Error e = BURROW_NO_ERROR;
    io_offset_writer_write_at(&w, BURROW_B("hello"), -1, &e);
    CHECK(BURROW_FAILED(e));
}

static void check_content(TestingT *t, MemFile *f) {
    Byte buf[CONTENT_SIZE + 1];
    Error err = BURROW_NO_ERROR;
    Int read_n = mem_file_read_at(
        f, slice_from(buf, CONTENT_SIZE + 1, CONTENT_SIZE + 1, TYPE_BYTE), 0, &err);
    CHECK(errors_is(err, io_eof));
    CHECK_INT_EQ(read_n, CONTENT_SIZE);
    CHECK(memcmp(buf, content, (size_t)CONTENT_SIZE) == 0);
}

static void TestOffsetWriter_Write(TestingT *t) {
    /* Write */
    static MemFile f, f2, f3;
    IoOffsetWriter w = io_new_offset_writer(mem_file_writer_at(&f), 0);
    for (Int i = 0; i < CONTENT_SIZE; i++) {
        Byte v = (Byte)content[i];
        Error err = BURROW_NO_ERROR;
        io_offset_writer_write(&w, slice_from(&v, 1, 1, TYPE_BYTE), &err);
        CHECK(BURROW_OK(err));
    }
    check_content(t, &f);

    /* Copy */
    IoOffsetWriter w2 = io_new_offset_writer(mem_file_writer_at(&f2), 0);
    IoSectionReader fr = io_new_section_reader(mem_file_reader_at(&f), 0, f.len);
    io_copy(heap(), io_offset_writer_as_io_writer(&w2),
            io_section_reader_as_io_reader(&fr), NULL);
    check_content(t, &f2);

    /* Write_Of_Copy_WriteTo */
    StringsReader sr;
    strings_reader_reset(&sr, str_from_cstr(content));
    IoOffsetWriter w3 = io_new_offset_writer(mem_file_writer_at(&f3), 0);
    io_copy(heap(), io_offset_writer_as_io_writer(&w3),
            strings_reader_as_io_reader(&sr), NULL);
    check_content(t, &f3);
}

/* ------------------------------------------------ the rest of the surface */

static void TestReadAllGrowsPastOneBlock(TestingT *t) {
    Byte big[3000];
    for (Int i = 0; i < LEN(big); i++)
        big[i] = (Byte)('a' + i % 26);
    Buffer rb = new_buffer();
    bytes_buffer_write(&rb.b, slice_from(big, LEN(big), LEN(big), TYPE_BYTE), NULL);
    Error err = BURROW_NO_ERROR;
    Slice got = io_read_all(heap(), buffer_as_io_reader(&rb), &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(got.len, LEN(big));
    CHECK(memcmp(got.p, big, sizeof big) == 0);
    mem_free(heap(), got.p, (size_t)got.cap, 1);
    bytes_buffer_free(&rb.b);
}

static void TestWriteStringUsesStringWriter(TestingT *t) {
    /* A builder hands out its buffer and never frees it, so it gets an arena. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder sb = STRINGS_BUILDER(arena_allocator(&ar));
    Error err = BURROW_NO_ERROR;
    Int n = io_write_string(strings_builder_as_io_writer(&sb), S("hello"), &err);
    CHECK_INT_EQ(n, 5);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(strings_builder_string(&sb), S("hello")));
    arena_free(&ar);

    Buffer wb = new_buffer();
    n = io_write_string(buffer_as_io_writer(&wb), S("plain"), &err);
    CHECK_INT_EQ(n, 5);
    CHECK(holds(&wb.b, "plain"));
    bytes_buffer_free(&wb.b);
}

static void TestDiscard(TestingT *t) {
    StringsReader sr;
    strings_reader_reset(&sr, S("into the void"));
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(heap(), io_discard, strings_reader_as_io_reader(&sr), &err);
    CHECK(n == 13 && BURROW_OK(err));

    Buffer rb = new_buffer();
    bytes_buffer_write_string(&rb.b, S("through ReadFrom"), NULL);
    n = io_copy(heap(), io_discard, buffer_as_io_reader(&rb), &err);
    CHECK(n == 16 && BURROW_OK(err));
    CHECK_INT_EQ(io_write_string(io_discard, S("abc"), &err), 3);
    bytes_buffer_free(&rb.b);
}

static void TestLimitedReaderStopsAtN(TestingT *t) {
    StringsReader sr;
    strings_reader_reset(&sr, S("hello, world."));
    IoLimitedReader l = io_limit_reader(strings_reader_as_io_reader(&sr), 5);
    Error err = BURROW_NO_ERROR;
    Slice got = io_read_all(heap(), io_limited_reader_as_io_reader(&l), &err);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(str_from_bytes((const Byte *)got.p, got.len), S("hello")));
    CHECK(l.n == 0);
    mem_free(heap(), got.p, (size_t)got.cap, 1);
}

#define TESTS(X)                                                                       \
    X(TestCopy)                                                                        \
    X(TestCopyNegative)                                                                \
    X(TestCopyBuffer)                                                                  \
    X(TestCopyBufferNil)                                                               \
    X(TestCopyReadFrom)                                                                \
    X(TestCopyWriteTo)                                                                 \
    X(TestCopyPriority)                                                                \
    X(TestCopyReadErrWriteErr)                                                         \
    X(TestCopyN)                                                                       \
    X(TestCopyNReadFrom)                                                               \
    X(TestCopyNWriteTo)                                                                \
    X(TestCopyNEOF)                                                                    \
    X(TestReadAtLeast)                                                                 \
    X(TestReadAtLeastWithDataAndEOF)                                                   \
    X(TestReadAtLeastWithDataAndError)                                                 \
    X(TestTeeReader)                                                                   \
    X(TestSectionReader_ReadAt)                                                        \
    X(TestSectionReader_Seek)                                                          \
    X(TestSectionReader_Size)                                                          \
    X(TestSectionReader_Max)                                                           \
    X(TestCopyLargeWriter)                                                             \
    X(TestNopCloserWriterToForwarding)                                                 \
    X(TestOffsetWriter_Seek)                                                           \
    X(TestOffsetWriter_WriteAt)                                                        \
    X(TestWriteAt_PositionPriorToBase)                                                 \
    X(TestOffsetWriter_Write)                                                          \
    X(TestReadAllGrowsPastOneBlock)                                                    \
    X(TestWriteStringUsesStringWriter)                                                 \
    X(TestDiscard)                                                                     \
    X(TestLimitedReaderStopsAtN)

TESTING_MAIN(TESTS)
