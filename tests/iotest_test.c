/* Derived from Go's src/testing/iotest/reader_test.go, writer_test.go,
 * logger_test.go and example_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2019 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/iotest.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* Go compares with ==, and an Error is two words. */
static bool same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Slice nil_bytes(void) {
    return slice_from(NULL, 0, 0, TYPE_BYTE);
}

static void TestOneByteReader_nonEmptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_string(&buf, S("Hello, World!"), NULL);

    IoReader obr = iotest_one_byte_reader(a, bytes_buffer_as_io_reader(&buf));
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(obr, read, nil_bytes(), &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);

    Byte b[3];
    StringsBuilder got = STRINGS_BUILDER(a);
    for (Int i = 0;; i++) {
        n = BURROW_CALL(obr, read, slice_from(b, 3, 3, TYPE_BYTE), &err);
        if (BURROW_FAILED(err))
            break;
        if (n != 1)
            testing_t_errorf_v(t, "Iteration #%d read %d bytes, want %d", i, n, (Int)1);
        strings_builder_write(&got, slice_from(b, n, n, TYPE_BYTE), NULL);
    }
    if (!same(err, io_eof))
        testing_t_errorf_v(
            t, "Unexpected error after reading all bytes\n\tGot:  %v\n\tWant: %v", err,
            io_eof);
    if (!str_eq(strings_builder_string(&got), S("Hello, World!")))
        testing_t_errorf_v(t, "Read mismatch\n\tGot:  %q\n\tWant: %q",
                           strings_builder_string(&got), S("Hello, World!"));
    arena_free(&ar);
}

static void TestOneByteReader_emptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer r = BYTES_BUFFER(a);

    IoReader obr = iotest_one_byte_reader(a, bytes_buffer_as_io_reader(&r));
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(obr, read, nil_bytes(), &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);

    Byte b[5];
    n = BURROW_CALL(obr, read, slice_from(b, 5, 5, TYPE_BYTE), &err);
    if (!same(err, io_eof))
        testing_t_errorf_v(t, "Error mismatch\n\tGot:  %v\n\tWant: %v", err, io_eof);
    if (n != 0)
        testing_t_errorf_v(t, "Unexpectedly read %d bytes, wanted %d", n, (Int)0);
    arena_free(&ar);
}

static void TestHalfReader_nonEmptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_string(&buf, S("Hello, World!"), NULL);

    IoReader hr = iotest_half_reader(a, bytes_buffer_as_io_reader(&buf));
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(hr, read, nil_bytes(), &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);

    Byte b[2];
    StringsBuilder got = STRINGS_BUILDER(a);
    for (Int i = 0;; i++) {
        n = BURROW_CALL(hr, read, slice_from(b, 2, 2, TYPE_BYTE), &err);
        if (BURROW_FAILED(err))
            break;
        if (n != 1)
            testing_t_errorf_v(t, "Iteration #%d read %d bytes, want %d", i, n, (Int)1);
        strings_builder_write(&got, slice_from(b, n, n, TYPE_BYTE), NULL);
    }
    if (!same(err, io_eof))
        testing_t_errorf_v(
            t, "Unexpected error after reading all bytes\n\tGot:  %v\n\tWant: %v", err,
            io_eof);
    if (!str_eq(strings_builder_string(&got), S("Hello, World!")))
        testing_t_errorf_v(t, "Read mismatch\n\tGot:  %q\n\tWant: %q",
                           strings_builder_string(&got), S("Hello, World!"));
    arena_free(&ar);
}

static void TestHalfReader_emptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer r = BYTES_BUFFER(a);

    IoReader hr = iotest_half_reader(a, bytes_buffer_as_io_reader(&r));
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(hr, read, nil_bytes(), &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);

    Byte b[5];
    n = BURROW_CALL(hr, read, slice_from(b, 5, 5, TYPE_BYTE), &err);
    if (!same(err, io_eof))
        testing_t_errorf_v(t, "Error mismatch\n\tGot:  %v\n\tWant: %v", err, io_eof);
    if (n != 0)
        testing_t_errorf_v(t, "Unexpectedly read %d bytes, wanted %d", n, (Int)0);
    arena_free(&ar);
}

static void TestTimeOutReader_nonEmptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_string(&buf, S("Hello, World!"), NULL);

    IoReader tor = iotest_timeout_reader(a, bytes_buffer_as_io_reader(&buf));
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(tor, read, nil_bytes(), &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);
    n = BURROW_CALL(tor, read, nil_bytes(), &err);
    if (!same(err, iotest_err_timeout))
        testing_t_errorf_v(t, "Error mismatch\n\tGot:  %v\n\tWant: %v", err,
                           iotest_err_timeout);
    if (n != 0)
        testing_t_errorf_v(t, "Unexpectedly read %d bytes, wanted %d", n, (Int)0);

    IoReader tor2 = iotest_timeout_reader(a, bytes_buffer_as_io_reader(&buf));
    Byte b[3];
    n = BURROW_CALL(tor2, read, slice_from(b, 3, 3, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || n == 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);
    n = BURROW_CALL(tor2, read, slice_from(b, 3, 3, TYPE_BYTE), &err);
    if (!same(err, iotest_err_timeout))
        testing_t_errorf_v(t, "Error mismatch\n\tGot:  %v\n\tWant: %v", err,
                           iotest_err_timeout);
    if (n != 0)
        testing_t_errorf_v(t, "Unexpectedly read %d bytes, wanted %d", n, (Int)0);
    arena_free(&ar);
}

static void TestTimeOutReader_emptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer r = BYTES_BUFFER(a);

    IoReader tor = iotest_timeout_reader(a, bytes_buffer_as_io_reader(&r));
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(tor, read, nil_bytes(), &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);
    n = BURROW_CALL(tor, read, nil_bytes(), &err);
    if (!same(err, iotest_err_timeout))
        testing_t_errorf_v(t, "Error mismatch\n\tGot:  %v\n\tWant: %v", err,
                           iotest_err_timeout);
    if (n != 0)
        testing_t_errorf_v(t, "Unexpectedly read %d bytes, wanted %d", n, (Int)0);

    IoReader tor2 = iotest_timeout_reader(a, bytes_buffer_as_io_reader(&r));
    Byte b[5];
    n = BURROW_CALL(tor2, read, slice_from(b, 5, 5, TYPE_BYTE), &err);
    if (!same(err, io_eof) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);
    n = BURROW_CALL(tor2, read, slice_from(b, 5, 5, TYPE_BYTE), &err);
    if (!same(err, iotest_err_timeout))
        testing_t_errorf_v(t, "Error mismatch\n\tGot:  %v\n\tWant: %v", err,
                           iotest_err_timeout);
    if (n != 0)
        testing_t_errorf_v(t, "Unexpectedly read %d bytes, wanted %d", n, (Int)0);
    arena_free(&ar);
}

static void TestDataErrReader_nonEmptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_string(&buf, S("Hello, World!"), NULL);

    IoReader der = iotest_data_err_reader(a, bytes_buffer_as_io_reader(&buf));
    Byte b[3];
    StringsBuilder got = STRINGS_BUILDER(a);
    Int n;
    Error err = BURROW_NO_ERROR;
    for (;;) {
        n = BURROW_CALL(der, read, slice_from(b, 3, 3, TYPE_BYTE), &err);
        strings_builder_write(&got, slice_from(b, n, n, TYPE_BYTE), NULL);
        if (BURROW_FAILED(err))
            break;
    }
    if (!same(err, io_eof) || n == 0)
        testing_t_errorf_v(t, "Last Read returned n=%d err=%v", n, err);
    if (!str_eq(strings_builder_string(&got), S("Hello, World!")))
        testing_t_errorf_v(t, "Read mismatch\n\tGot:  %q\n\tWant: %q",
                           strings_builder_string(&got), S("Hello, World!"));
    iotest_reader_free(a, der);
    arena_free(&ar);
}

static void TestDataErrReader_emptyReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer r = BYTES_BUFFER(a);

    IoReader der = iotest_data_err_reader(a, bytes_buffer_as_io_reader(&r));
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(der, read, nil_bytes(), &err);
    if (!same(err, io_eof) || n != 0)
        testing_t_errorf_v(t, "Empty buffer read returned n=%d err=%v", n, err);

    Byte b[5];
    n = BURROW_CALL(der, read, slice_from(b, 5, 5, TYPE_BYTE), &err);
    if (!same(err, io_eof))
        testing_t_errorf_v(t, "Error mismatch\n\tGot:  %v\n\tWant: %v", err, io_eof);
    if (n != 0)
        testing_t_errorf_v(t, "Unexpectedly read %d bytes, wanted %d", n, (Int)0);
    arena_free(&ar);
}

typedef struct ErrReaderCase {
    const char *name;
    Error err;
} ErrReaderCase;

static void err_reader_case(void *env, TestingT *t) {
    const ErrReaderCase *tt = (const ErrReaderCase *)env;
    IoReader r = iotest_err_reader(heap_allocator(), tt->err);
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(r, read, nil_bytes(), &err);
    iotest_reader_free(heap_allocator(), r);
    if (!same(err, tt->err))
        testing_t_fatalf_v(t, "Error mismatch\nGot:  %v\nWant: %v", err, tt->err);
    if (n != 0)
        testing_t_fatalf_v(t, "Byte count mismatch: got %d want 0", n);
}

static void TestErrReader(TestingT *t) {
    ErrReaderCase cases[] = {
        {"nil error", BURROW_NO_ERROR},
        {"non-nil error", errors_new(error_allocator(), S("io failure"))},
        {"io.EOF", io_eof},
    };
    for (Int i = 0; i < LEN(cases); i++)
        testing_t_run(t, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingTFunc, err_reader_case, &cases[i]));
}

static void TestStringsReader(TestingT *t) {
    Str msg = S("Now is the time for all good gophers.");
    StringsReader r;
    strings_reader_reset(&r, msg);
    Error err = iotest_test_reader(strings_reader_as_io_reader(&r), bytes_of(msg));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

/* Not in Go's tests, which only try a strings.Reader. These give the Seek
 * and ReadAt checks a bytes.Reader and a file too, and an empty input. */
static void TestBytesReader(TestingT *t) {
    const char *inputs[] = {"", "a", "ab", "Now is the time for all good gophers."};
    for (Int i = 0; i < LEN(inputs); i++) {
        Slice msg = bytes_of(str_from_cstr(inputs[i]));
        BytesReader r;
        bytes_reader_reset(&r, msg);
        Error err = iotest_test_reader(bytes_reader_as_io_reader(&r), msg);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%q: %v", str_from_cstr(inputs[i]), err);
    }
}

static void TestFileReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str msg = S("Now is the time for all good gophers.");
    Error err = BURROW_NO_ERROR;
    OsFile *f = os_create_temp(a, BURROW_STR_EMPTY, S("iotest"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str name = os_file_name(f);
    os_file_write(f, bytes_of(msg), &err);
    if (BURROW_OK(err))
        os_file_seek(f, 0, 0, &err);
    if (BURROW_OK(err))
        err = iotest_test_reader(os_file_as_io_reader(f), bytes_of(msg));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    (void)os_file_close(f);
    (void)os_remove(name);
    os_file_free(f);
    arena_free(&ar);
}

/* And that what it reports reads the way Go's does. */
static void TestReaderReports(TestingT *t) {
    StringsReader r;
    strings_reader_reset(&r, S("abc"));
    Error err = iotest_test_reader(strings_reader_as_io_reader(&r), bytes_of(S("abd")));
    Str want = S("ReadAll(small amounts) = \"abc\"\n\twant \"abd\"");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "got %q, want %q", error_text(err), want);

    IoReader er =
        iotest_err_reader(heap_allocator(), errors_new(error_allocator(), S("boom")));
    err = iotest_test_reader(er, bytes_of(S("")));
    iotest_reader_free(heap_allocator(), er);
    want = S("Read(0 bytes at offset 0): boom");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "got %q, want %q", error_text(err), want);
}

typedef struct TruncateWriterTest {
    const char *in;
    const char *want;
    int64_t trunc;
    Int n;
} TruncateWriterTest;

static const TruncateWriterTest truncate_writer_tests[] = {
    {"hello", "", -1, 5},
    {"world", "", 0, 5},
    {"abcde", "abc", 3, 5},
    {"edcba", "edcba", 7, 5},
};

static void TestTruncateWriter(TestingT *t) {
    for (Int i = 0; i < LEN(truncate_writer_tests); i++) {
        const TruncateWriterTest *tt = &truncate_writer_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        StringsBuilder buf = STRINGS_BUILDER(a);
        IoWriter tw =
            iotest_truncate_writer(a, strings_builder_as_io_writer(&buf), tt->trunc);
        Error err = BURROW_NO_ERROR;
        Int n = BURROW_CALL(tw, write, bytes_of(str_from_cstr(tt->in)), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Unexpected error %v for\n\t%s", err,
                               str_from_cstr(tt->in));
        if (!str_eq(strings_builder_string(&buf), str_from_cstr(tt->want)))
            testing_t_errorf_v(t, "got %q, expected %q", strings_builder_string(&buf),
                               str_from_cstr(tt->want));
        if (n != tt->n)
            testing_t_errorf_v(
                t, "read %d bytes, but expected to have read %d bytes for\n\t%s", n,
                tt->n, str_from_cstr(tt->in));
        iotest_writer_free(a, tw);
        arena_free(&ar);
    }
}

/* The standard logger's writer, flags and prefix, which the logger tests
 * change and put back. */
typedef struct LogSettings {
    IoWriter w;
    Int flags;
    Str prefix;
} LogSettings;

static LogSettings log_settings_save(StringsBuilder *out, Str prefix) {
    LogSettings old = {log_writer(), log_flags(), log_prefix()};
    log_set_prefix(prefix);
    log_set_output(strings_builder_as_io_writer(out));
    log_set_flags(0);
    return old;
}

static void log_settings_restore(LogSettings old) {
    log_set_flags(old.flags);
    log_set_prefix(old.prefix);
    log_set_output(old.w);
}

/* Go's errWriter. */
static Int err_write(void *self, Slice p, Error *err) {
    (void)p;
    BURROW_OUT(err, *(const Error *)self);
    return 0;
}

static const IoWriterVT err_writer_vt = {NULL, err_write};

static void TestWriteLogger(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder l_out = STRINGS_BUILDER(a);
    LogSettings old = log_settings_save(&l_out, S("lw: "));

    StringsBuilder lw = STRINGS_BUILDER(a);
    IoWriter wl =
        iotest_new_write_logger(a, S("write:"), strings_builder_as_io_writer(&lw));
    Error err = BURROW_NO_ERROR;
    BURROW_CALL(wl, write, bytes_of(S("Hello, World!")), &err);
    if (BURROW_FAILED(err)) {
        log_settings_restore(old);
        testing_t_fatalf_v(t, "Unexpectedly failed to write: %v", err);
    }

    Str g = strings_builder_string(&lw), w = S("Hello, World!");
    if (!str_eq(g, w))
        testing_t_errorf_v(t, "WriteLogger mismatch\n\tgot:  %q\n\twant: %q", g, w);
    Str want_log_with_hex = fmt_sprintf_v(a, "lw: write: %x\n", S("Hello, World!"));
    g = strings_builder_string(&l_out);
    if (!str_eq(g, want_log_with_hex))
        testing_t_errorf_v(t, "WriteLogger mismatch\n\tgot:  %q\n\twant: %q", g,
                           want_log_with_hex);
    log_settings_restore(old);
    arena_free(&ar);
}

static void TestWriteLogger_errorOnWrite(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder l_out = STRINGS_BUILDER(a);
    LogSettings old = log_settings_save(&l_out, S("lw: "));

    Error write_err = errors_new(a, S("Write Error!"));
    IoWriter lw = {&err_writer_vt, &write_err};
    IoWriter wl = iotest_new_write_logger(a, S("write:"), lw);
    Error err = BURROW_NO_ERROR;
    BURROW_CALL(wl, write, bytes_of(S("Hello, World!")), &err);
    if (BURROW_OK(err)) {
        log_settings_restore(old);
        testing_t_fatalf_v(t, "Unexpectedly succeeded to write: %v", err);
    }

    Str want_log_with_hex =
        fmt_sprintf_v(a, "lw: write: %x: %v\n", S(""), S("Write Error!"));
    Str g = strings_builder_string(&l_out);
    if (!str_eq(g, want_log_with_hex))
        testing_t_errorf_v(t, "WriteLogger mismatch\n\tgot:  %q\n\twant: %q", g,
                           want_log_with_hex);
    log_settings_restore(old);
    arena_free(&ar);
}

static void TestReadLogger(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder l_out = STRINGS_BUILDER(a);
    LogSettings old = log_settings_save(&l_out, S("lr: "));

    Slice data = bytes_of(S("Hello, World!"));
    Slice p = slice_make(a, TYPE_BYTE, data.len, data.len);
    BytesReader *lr = bytes_new_reader(a, data);
    IoReader rl = iotest_new_read_logger(a, S("read:"), bytes_reader_as_io_reader(lr));

    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(rl, read, p, &err);
    if (BURROW_FAILED(err)) {
        log_settings_restore(old);
        testing_t_fatalf_v(t, "Unexpectedly failed to read: %v", err);
    }

    Slice g = slice_sub(p, 0, n);
    if (!bytes_equal(g, data))
        testing_t_errorf_v(t, "ReadLogger mismatch\n\tgot:  %q\n\twant: %q", g, data);
    Str want_log_with_hex = fmt_sprintf_v(a, "lr: read: %x\n", S("Hello, World!"));
    Str gs = strings_builder_string(&l_out);
    if (!str_eq(gs, want_log_with_hex))
        testing_t_errorf_v(t, "ReadLogger mismatch\n\tgot:  %q\n\twant: %q", gs,
                           want_log_with_hex);
    log_settings_restore(old);
    arena_free(&ar);
}

static void TestReadLogger_errorOnRead(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder l_out = STRINGS_BUILDER(a);
    LogSettings old = log_settings_save(&l_out, S("lr: "));

    Slice data = bytes_of(S("Hello, World!"));
    Slice p = slice_make(a, TYPE_BYTE, data.len, data.len);

    IoReader lr = iotest_err_reader(a, errors_new(a, S("io failure")));
    IoReader rl = iotest_new_read_logger(a, S("read"), lr);
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(rl, read, p, &err);
    if (BURROW_OK(err)) {
        log_settings_restore(old);
        testing_t_fatalf_v(t, "Unexpectedly succeeded to read: %v", err);
    }

    Str want_log_with_hex =
        fmt_sprintf_v(a, "lr: read %x: io failure\n", slice_sub(p, 0, n));
    Str g = strings_builder_string(&l_out);
    if (!str_eq(g, want_log_with_hex))
        testing_t_errorf_v(t, "ReadLogger mismatch\n\tgot:  %q\n\twant: %q", g,
                           want_log_with_hex);
    log_settings_restore(old);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestOneByteReader_nonEmptyReader)                                                \
    X(TestOneByteReader_emptyReader)                                                   \
    X(TestHalfReader_nonEmptyReader)                                                   \
    X(TestHalfReader_emptyReader)                                                      \
    X(TestTimeOutReader_nonEmptyReader)                                                \
    X(TestTimeOutReader_emptyReader)                                                   \
    X(TestDataErrReader_nonEmptyReader)                                                \
    X(TestDataErrReader_emptyReader)                                                   \
    X(TestErrReader)                                                                   \
    X(TestStringsReader)                                                               \
    X(TestBytesReader)                                                                 \
    X(TestFileReader)                                                                  \
    X(TestReaderReports)                                                               \
    X(TestTruncateWriter)                                                              \
    X(TestWriteLogger)                                                                 \
    X(TestWriteLogger_errorOnWrite)                                                    \
    X(TestReadLogger)                                                                  \
    X(TestReadLogger_errorOnRead)

TESTING_MAIN(TESTS)
