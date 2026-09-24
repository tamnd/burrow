/* Derived from Go's src/io/multi_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/hash.h"
#include "burrow/hash/crc32.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"

#include "../src/io/internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

static const Type test_type = {
    {(const Byte *)"testType", 8},
    {(const Byte *)"io_test", 7},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74657374U,
    NULL,
};

/* ------------------------------------------------------------ MultiReader */

typedef struct FooBar {
    StringsReader r1, r2, r3;
    IoReader mr;
    Byte bufa[20];
    Slice buf;
    int nread;
} FooBar;

static void with_foo_bar(FooBar *fb) {
    strings_reader_reset(&fb->r1, S("foo "));
    strings_reader_reset(&fb->r2, S(""));
    strings_reader_reset(&fb->r3, S("bar"));
    IoReader rs[3] = {strings_reader_as_io_reader(&fb->r1),
                      strings_reader_as_io_reader(&fb->r2),
                      strings_reader_as_io_reader(&fb->r3)};
    fb->mr = io_multi_reader(a, rs, 3);
    fb->buf = slice_from(fb->bufa, 20, 20, TYPE_BYTE);
}

static void expect_read(TestingT *t, FooBar *fb, Int size, const char *expected,
                        bool eof) {
    fb->nread++;
    Error gerr = BURROW_NO_ERROR;
    Int n = BURROW_CALL(fb->mr, read, slice_sub(fb->buf, 0, size), &gerr);
    Str want = str_from_cstr(expected);
    if (n != want.len)
        testing_t_errorf_v(t, "#%d, expected %d bytes; got %d", fb->nread, want.len, n);
    Str got = str_from_bytes((const Byte *)fb->buf.p, n);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "#%d, expected %q; got %q", fb->nread, want, got);
    if (eof ? !errors_is(gerr, io_eof) : BURROW_FAILED(gerr))
        testing_t_errorf_v(t, "#%d, expected error %v; got %v", fb->nread,
                           eof ? io_eof : BURROW_NO_ERROR, gerr);
    fb->buf = slice_sub(fb->buf, n, fb->buf.len);
}

static void TestMultiReader(TestingT *t) {
    FooBar fb = {0};
    with_foo_bar(&fb);
    expect_read(t, &fb, 2, "fo", false);
    expect_read(t, &fb, 5, "o ", false);
    expect_read(t, &fb, 5, "bar", false);
    expect_read(t, &fb, 5, "", true);

    with_foo_bar(&fb);
    expect_read(t, &fb, 4, "foo ", false);
    expect_read(t, &fb, 1, "b", false);
    expect_read(t, &fb, 3, "ar", false);
    expect_read(t, &fb, 1, "", true);

    with_foo_bar(&fb);
    expect_read(t, &fb, 5, "foo ", false);
}

static void TestMultiReaderAsWriterTo(TestingT *t) {
    StringsReader r1, r2, r3;
    strings_reader_reset(&r1, S("foo "));
    strings_reader_reset(&r2, S(""));
    strings_reader_reset(&r3, S("bar"));
    /* Tickle the buffer reusing codepath. */
    IoReader inner[2] = {strings_reader_as_io_reader(&r2),
                         strings_reader_as_io_reader(&r3)};
    IoReader outer[2] = {strings_reader_as_io_reader(&r1),
                         io_multi_reader(a, inner, 2)};
    IoReader mr = io_multi_reader(a, outer, 2);

    const Method *m = type_method_by_name(mr.vt->self_type, S("WriteTo"));
    if (m == NULL) {
        testing_t_fatal_v(t, "expected cast to WriterTo to succeed");
        return;
    }
    StringsBuilder sink = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&sink);
    Error err = BURROW_NO_ERROR;
    Error *errp = &err;
    int64_t n = 0;
    void *args[2] = {&w, (void *)&errp};
    void *rets[1] = {&n};
    CHECK(method_call(m, mr.data, args, rets));
    CHECK(BURROW_OK(err));
    CHECK(n == 7);
    CHECK(str_eq(strings_builder_string(&sink), S("foo bar")));
}

/* ------------------------------------------------------------ MultiWriter */

static void test_multi_writer(TestingT *t, IoWriter sink, Str (*sink_string)(void *),
                              void *sink_data) {
    HashHash32 crc = crc32_new_ieee(a);
    IoWriter ws[2] = {hash_as_io_writer(hash_hash32_as_hash(crc)), sink};
    IoWriter mw = io_multi_writer(a, ws, 2);
    static const char source_string[] = "My input text.";
    StringsReader source;
    strings_reader_reset(&source, str_from_cstr(source_string));
    Error err = BURROW_NO_ERROR;
    int64_t written = io_copy(a, mw, strings_reader_as_io_reader(&source), &err);
    if (written != (int64_t)(sizeof source_string - 1))
        testing_t_errorf_v(t, "short write of %d, not %d", written,
                           (Int)(sizeof source_string - 1));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error: %v", err);
    /* Go checks a sha1 here, which is not ported yet. A crc of the same text
     * shows the same thing, that the first writer saw every byte. */
    if (hash_hash32_sum32(crc) != 0x204aab43U)
        testing_t_error_v(t, "incorrect crc32 value");
    Str got = sink_string(sink_data);
    if (!str_eq(got, str_from_cstr(source_string)))
        testing_t_errorf_v(t, "expected %q; got %q", str_from_cstr(source_string), got);
}

static Str buffer_string(void *b) {
    Slice s = bytes_buffer_bytes((BytesBuffer *)b);
    return str_from_bytes((const Byte *)s.p, s.len);
}

static void TestMultiWriter(TestingT *t) {
    BytesBuffer sink = BYTES_BUFFER(a);
    test_multi_writer(t, bytes_buffer_as_io_writer(&sink), buffer_string, &sink);
}

typedef struct WriteStringChecker {
    bool called;
} WriteStringChecker;

static Int write_string_checker_write_string(WriteStringChecker *c, Str s, Error *err) {
    c->called = true;
    *err = BURROW_NO_ERROR;
    return s.len;
}

static Int write_string_checker_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)err;
    return p.len;
}

#define WRITE_STRING_CHECKER_METHODS(M, T)                                             \
    M(T, WriteString, write_string_checker_write_string, IO_SIG_WRITE_STRING)
BURROW_METHODS_DEFINE(WriteStringChecker, WRITE_STRING_CHECKER_METHODS);

static const Type write_string_checker_type = {
    {(const Byte *)"writeStringChecker", 18},
    {(const Byte *)"io_test", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(WriteStringChecker),
    (uint16_t)_Alignof(WriteStringChecker),
    0,
    (uint16_t)(sizeof burrow__methods_WriteStringChecker /
               sizeof burrow__methods_WriteStringChecker[0]),
    NULL,
    burrow__methods_WriteStringChecker,
    NULL,
    NULL,
    0,
    0x77736368U,
    NULL,
};

static const IoWriterVT write_string_checker_vt = {&write_string_checker_type,
                                                   write_string_checker_write};

static void TestMultiWriter_StringCheckCall(TestingT *t) {
    WriteStringChecker c = {false};
    IoWriter cw = {&write_string_checker_vt, &c};
    IoWriter mw = io_multi_writer(a, &cw, 1);
    io_write_string(mw, S("foo"), NULL);
    if (!c.called)
        testing_t_error_v(t, "did not see WriteString call to writeStringChecker");
}

/* writerFunc: the data is the function to call. */
typedef Int (*WriteFn)(Slice p, Error *err);

typedef struct WriterFunc {
    WriteFn f;
} WriterFunc;

static Int writer_func_write(void *self, Slice p, Error *err) {
    return ((WriterFunc *)self)->f(p, err);
}

static const IoWriterVT writer_func_vt = {&test_type, writer_func_write};

static Int calls;

static Int count_write(Slice p, Error *err) {
    (void)p;
    (void)err;
    calls++;
    return 0;
}

static void TestMultiWriterSingleChainFlatten(TestingT *t) {
    WriterFunc f = {count_write};
    IoWriter w0 = {&writer_func_vt, &f};
    IoWriter w = io_multi_writer(a, &w0, 1);
    IoWriter mw = w;
    for (int i = 0; i < 100; i++)
        mw = io_multi_writer(a, &w, 1);
    IoWriter four[4] = {w, mw, w, mw};
    mw = io_multi_writer(a, four, 4);
    calls = 0;
    BURROW_CALL(mw, write, slice_nil(TYPE_BYTE), &(Error){0});

    /* Go counts stack frames to see that the write went straight to the
     * function each time. Flattened, the outer list holds the function four
     * times and nothing else. */
    const MultiWriter *m = (const MultiWriter *)mw.data;
    CHECK_INT_EQ(m->n, 4);
    for (Int i = 0; i < m->n; i++)
        CHECK(m->writers[i].vt == &writer_func_vt && m->writers[i].data == &f);
    CHECK_INT_EQ(calls, 4);
}

static Int half_short_write(Slice p, Error *err) {
    *err = io_err_short_write;
    return p.len / 2;
}

static Int f2_called;

static Int f2_write(Slice p, Error *err) {
    (void)err;
    f2_called++;
    return p.len;
}

static void TestMultiWriterError(TestingT *t) {
    WriterFunc f1 = {half_short_write}, f2 = {f2_write};
    IoWriter ws[2] = {{&writer_func_vt, &f1}, {&writer_func_vt, &f2}};
    IoWriter w = io_multi_writer(a, ws, 2);
    Byte buf[100] = {0};
    Error err = BURROW_NO_ERROR;
    f2_called = 0;
    Int n = BURROW_CALL(w, write, slice_from(buf, 100, 100, TYPE_BYTE), &err);
    if (f2_called != 0)
        testing_t_error_v(t, "MultiWriter called f2.Write");
    if (n != 50 || !errors_is(err, io_err_short_write))
        testing_t_errorf_v(t, "Write = %d, %v, want 50, ErrShortWrite", n, err);
}

/* Test that MultiReader copies the input slice and is insulated from future
 * modification. */
static void TestMultiReaderCopy(TestingT *t) {
    StringsReader sr;
    strings_reader_reset(&sr, S("hello world"));
    IoReader slice[1] = {strings_reader_as_io_reader(&sr)};
    IoReader r = io_multi_reader(a, slice, 1);
    slice[0] = (IoReader){NULL, NULL};
    Error err = BURROW_NO_ERROR;
    Slice data = io_read_all(a, r, &err);
    Str got = str_from_bytes((const Byte *)data.p, data.len);
    if (BURROW_FAILED(err) || !str_eq(got, S("hello world")))
        testing_t_errorf_v(t, "ReadAll() = %q, %v, want %q, nil", got, err,
                           S("hello world"));
}

/* Test that MultiWriter copies the input slice and is insulated from future
 * modification. */
static void TestMultiWriterCopy(TestingT *t) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    IoWriter slice[1] = {strings_builder_as_io_writer(&buf)};
    IoWriter w = io_multi_writer(a, slice, 1);
    slice[0] = (IoWriter){NULL, NULL};
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(w, write, BURROW_B("hello world"), &err);
    if (BURROW_FAILED(err) || n != 11)
        testing_t_errorf_v(t, "Write(`hello world`) = %d, %v, want 11, nil", n, err);
    if (!str_eq(strings_builder_string(&buf), S("hello world")))
        testing_t_errorf_v(t, "buf.String() = %q, want %q",
                           strings_builder_string(&buf), S("hello world"));
}

/* readerFunc, the reading twin of writerFunc. */
typedef Int (*ReadFn)(Slice p, Error *err);

typedef struct ReaderFunc {
    ReadFn f;
} ReaderFunc;

static Int reader_func_read(void *self, Slice p, Error *err) {
    return ((ReaderFunc *)self)->f(p, err);
}

static const IoReaderVT reader_func_vt = {&test_type, reader_func_read};

static const Str irrelevant_text = {(const Byte *)"irrelevant", 10};
static const Error irrelevant = {&burrow_sentinel_error_vt, &irrelevant_text};

static Int irrelevant_read(Slice p, Error *err) {
    (void)p;
    calls++;
    *err = irrelevant;
    return 0;
}

/* Test that MultiReader properly flattens chained multiReaders when Read is
 * called. */
static void TestMultiReaderFlatten(TestingT *t) {
    ReaderFunc f = {irrelevant_read};
    IoReader r0 = {&reader_func_vt, &f};
    IoReader r = io_multi_reader(a, &r0, 1);
    for (int i = 0; i < 100; i++)
        r = io_multi_reader(a, &r, 1);
    calls = 0;
    Error err = BURROW_NO_ERROR;
    BURROW_CALL(r, read, slice_nil(TYPE_BYTE), &err);
    CHECK(errors_is(err, irrelevant));
    CHECK_INT_EQ(calls, 1);

    /* Go counts stack frames again. After the read the outer reader has taken
     * over the innermost list, so a second read is one call deep. */
    const MultiReader *m = (const MultiReader *)r.data;
    CHECK_INT_EQ(m->n, 1);
    CHECK(m->readers[0].vt == &reader_func_vt && m->readers[0].data == &f);
}

/* byteAndEOFReader is a Reader which reads one byte (the underlying byte)
 * and EOF at once in its Read call. */
typedef struct ByteAndEofReader {
    Byte b;
} ByteAndEofReader;

static Int byte_and_eof_read(void *self, Slice p, Error *err) {
    if (p.len == 0)
        /* Read(0 bytes) is useless. We expect no such useless calls in this
         * test. */
        runtime_panic(S("unexpected call"));
    *(Byte *)p.p = ((ByteAndEofReader *)self)->b;
    *err = io_eof;
    return 1;
}

static const IoReaderVT byte_and_eof_vt = {&test_type, byte_and_eof_read};

/* This used to yield bytes forever; issue 16795. */
static void TestMultiReaderSingleByteWithEOF(TestingT *t) {
    ByteAndEofReader ra = {'a'}, rb = {'b'};
    IoReader rs[2] = {{&byte_and_eof_vt, &ra}, {&byte_and_eof_vt, &rb}};
    IoLimitedReader l = io_limit_reader(io_multi_reader(a, rs, 2), 10);
    Error err = BURROW_NO_ERROR;
    Slice got = io_read_all(a, io_limited_reader_as_io_reader(&l), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        return;
    }
    Str g = str_from_bytes((const Byte *)got.p, got.len);
    if (!str_eq(g, S("ab")))
        testing_t_errorf_v(t, "got %q; want %q", g, S("ab"));
}

/* Test that a reader returning (n, EOF) at the end of a MultiReader chain
 * continues to return EOF on its final read, rather than yielding a (0, EOF). */
static void TestMultiReaderFinalEOF(TestingT *t) {
    BytesReader empty;
    bytes_reader_reset(&empty, slice_nil(TYPE_BYTE));
    ByteAndEofReader ra = {'a'};
    IoReader rs[2] = {bytes_reader_as_io_reader(&empty), {&byte_and_eof_vt, &ra}};
    IoReader r = io_multi_reader(a, rs, 2);
    Byte buf[2];
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(r, read, slice_from(buf, 2, 2, TYPE_BYTE), &err);
    if (n != 1 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "got %v, %v; want 1, EOF", n, err);
}

/* Go's test waits for the collector to take buf1. With no collector the part
 * worth keeping is that the used up slot lets go of the reader. */
static void TestMultiReaderFreesExhaustedReaders(TestingT *t) {
    BytesReader buf1, buf2;
    bytes_reader_reset(&buf1, BURROW_B("foo"));
    bytes_reader_reset(&buf2, BURROW_B("bar"));
    IoReader rs[2] = {bytes_reader_as_io_reader(&buf1),
                      bytes_reader_as_io_reader(&buf2)};
    IoReader mr = io_multi_reader(a, rs, 2);
    const MultiReader *m = (const MultiReader *)mr.data;
    IoReader *first = m->readers;

    Byte buf[4];
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(mr, slice_from(buf, 4, 4, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || memcmp(buf, "foob", 4) != 0) {
        testing_t_fatalf_v(t, "ReadFull = %d (%q), %v; want 3, \"foo\", nil", n,
                           str_from_bytes(buf, n), err);
        return;
    }
    CHECK(first[0].data != &buf1);

    n = io_read_full(mr, slice_from(buf, 2, 2, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || memcmp(buf, "ar", 2) != 0)
        testing_t_fatalf_v(t, "ReadFull = %d (%q), %v; want 2, \"ar\", nil", n,
                           str_from_bytes(buf, n), err);
}

static void TestInterleavedMultiReader(TestingT *t) {
    StringsReader r1, r2;
    strings_reader_reset(&r1, S("123"));
    strings_reader_reset(&r2, S("45678"));
    IoReader rs[2] = {strings_reader_as_io_reader(&r1),
                      strings_reader_as_io_reader(&r2)};
    IoReader mr1 = io_multi_reader(a, rs, 2);
    IoReader mr2 = io_multi_reader(a, &mr1, 1);

    Byte buf[4];
    Slice s = slice_from(buf, 4, 4, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    /* Have mr2 use mr1's []Readers. Consume r1 (and return "123"). Consume
     * r2 (and return "4"). */
    Int n = io_read_full(mr2, s, &err);
    Str got = str_from_bytes(buf, n);
    if (!str_eq(got, S("1234")) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadFull(mr2) = (%q, %v), want (\"1234\", nil)", got,
                           err);

    /* Consume the rest of r2 via mr1. This should not panic even though mr2
     * cleared the reference to r1. */
    n = io_read_full(mr1, s, &err);
    got = str_from_bytes(buf, n);
    if (!str_eq(got, S("5678")) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadFull(mr1) = (%q, %v), want (\"5678\", nil)", got,
                           err);
}

/* The allocation side, which Go does not have: free gives back what the
 * constructor took. */
static void TestMultiFreeReturnsTheBlock(TestingT *t) {
    StringsReader sr;
    strings_reader_reset(&sr, S("x"));
    IoReader r0 = strings_reader_as_io_reader(&sr);
    IoReader mr = io_multi_reader(heap_allocator(), &r0, 1);
    CHECK(mr.vt != NULL);
    io_multi_reader_free(heap_allocator(), mr);

    BytesBuffer b = BYTES_BUFFER(a);
    IoWriter w0 = bytes_buffer_as_io_writer(&b);
    IoWriter mw = io_multi_writer(heap_allocator(), &w0, 1);
    CHECK(mw.vt != NULL);
    io_multi_writer_free(heap_allocator(), mw);
}

#define TESTS(X)                                                                       \
    X(TestMultiReader)                                                                 \
    X(TestMultiReaderAsWriterTo)                                                       \
    X(TestMultiWriter)                                                                 \
    X(TestMultiWriter_StringCheckCall)                                                 \
    X(TestMultiWriterSingleChainFlatten)                                               \
    X(TestMultiWriterError)                                                            \
    X(TestMultiReaderCopy)                                                             \
    X(TestMultiWriterCopy)                                                             \
    X(TestMultiReaderFlatten)                                                          \
    X(TestMultiReaderSingleByteWithEOF)                                                \
    X(TestMultiReaderFinalEOF)                                                         \
    X(TestMultiReaderFreesExhaustedReaders)                                            \
    X(TestInterleavedMultiReader)                                                      \
    X(TestMultiFreeReturnsTheBlock)

static int TestMain(TestingM *m) {
    setup();
    int code = testing_m_run(m);
    teardown();
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
