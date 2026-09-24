/* Derived from Go's src/bytes/reader_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define SI BURROW_S_INIT
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* Go compares errors by identity or by text, and so does this. */
static bool same_error(Error got, const Error *want_err, const char *want_text) {
    if (want_err != NULL)
        return errors_is(got, *want_err);
    if (want_text == NULL)
        return BURROW_OK(got);
    return BURROW_FAILED(got) && str_eq(error_text(got), str_from_cstr(want_text));
}

static void TestReader(TestingT *t) {
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("0123456789"));
    static const struct {
        int64_t off;
        Int seek;
        Int n;
        Str want;
        int64_t wantpos;
        bool readerr_eof;
        const char *seekerr;
    } tests[] = {
        {0, BURROW_IO_SEEK_START, 20, SI("0123456789"), 0, false, NULL},
        {1, BURROW_IO_SEEK_START, 1, SI("1"), 0, false, NULL},
        {1, BURROW_IO_SEEK_CURRENT, 2, SI("34"), 3, false, NULL},
        {-1, BURROW_IO_SEEK_START, 0, SI(""), 0, false,
         "bytes.Reader.Seek: negative position"},
        {(int64_t)1 << 33, BURROW_IO_SEEK_START, 0, SI(""), (int64_t)1 << 33, true,
         NULL},
        {1, BURROW_IO_SEEK_CURRENT, 0, SI(""), ((int64_t)1 << 33) + 1, true, NULL},
        {0, BURROW_IO_SEEK_START, 5, SI("01234"), 0, false, NULL},
        {0, BURROW_IO_SEEK_CURRENT, 5, SI("56789"), 0, false, NULL},
        {-1, BURROW_IO_SEEK_END, 1, SI("9"), 9, false, NULL},
    };
    Byte buf[32];
    for (Int i = 0; i < LEN(tests); i++) {
        Error err;
        int64_t pos = bytes_reader_seek(&r, tests[i].off, tests[i].seek, &err);
        if (BURROW_OK(err) && tests[i].seekerr != NULL) {
            testing_t_errorf_v(t, "%d. want seek error %q", i, tests[i].seekerr);
            continue;
        }
        if (BURROW_FAILED(err) && !same_error(err, NULL, tests[i].seekerr)) {
            testing_t_errorf_v(t, "%d. seek error = %q; want %q", i, error_text(err),
                               tests[i].seekerr);
            continue;
        }
        if (tests[i].wantpos != 0 && tests[i].wantpos != pos)
            testing_t_errorf_v(t, "%d. pos = %d, want %d", i, pos, tests[i].wantpos);
        Int n = bytes_reader_read(
            &r, slice_from(buf, tests[i].n, tests[i].n, TYPE_BYTE), &err);
        if (!same_error(err, tests[i].readerr_eof ? &io_eof : NULL, NULL)) {
            testing_t_errorf_v(t, "%d. read = %v; want %v", i, err,
                               tests[i].readerr_eof ? io_eof : BURROW_NO_ERROR);
            continue;
        }
        Str got = str_from_bytes(buf, n);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "%d. got %q; want %q", i, got, tests[i].want);
    }
}

static void TestReadAfterBigSeek(TestingT *t) {
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("0123456789"));
    Error err;
    bytes_reader_seek(&r, ((int64_t)1 << 31) + 5, BURROW_IO_SEEK_START, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Byte buf[10];
    Int n = bytes_reader_read(&r, slice_from(buf, 10, 10, TYPE_BYTE), &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "Read = %d, %v; want 0, EOF", n, err);
}

static void TestReaderAt(TestingT *t) {
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("0123456789"));
    static const struct {
        int64_t off;
        Int n;
        Str want;
        bool eof;
        const char *wanterr;
    } tests[] = {
        {0, 10, SI("0123456789"), false, NULL},
        {1, 10, SI("123456789"), true, NULL},
        {1, 9, SI("123456789"), false, NULL},
        {11, 10, SI(""), true, NULL},
        {0, 0, SI(""), false, NULL},
        {-1, 0, SI(""), false, "bytes.Reader.ReadAt: negative offset"},
    };
    Byte b[16];
    for (Int i = 0; i < LEN(tests); i++) {
        Error err;
        Int rn = bytes_reader_read_at(
            &r, slice_from(b, tests[i].n, tests[i].n, TYPE_BYTE), tests[i].off, &err);
        Str got = str_from_bytes(b, rn);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "%d. got %q; want %q", i, got, tests[i].want);
        if (!same_error(err, tests[i].eof ? &io_eof : NULL, tests[i].wanterr))
            testing_t_errorf_v(t, "%d. got error = %v; want %s", i, err,
                               tests[i].eof       ? "EOF"
                               : tests[i].wanterr ? tests[i].wanterr
                                                  : "<nil>");
    }
}

typedef struct ReadAtJob {
    BytesReader *r;
    int64_t i;
} ReadAtJob;

static void read_at_one(void *env) {
    ReadAtJob *j = (ReadAtJob *)env;
    Byte buf[1];
    bytes_reader_read_at(j->r, slice_from(buf, 1, 1, TYPE_BYTE), j->i, NULL);
}

/* Test for the race detector, to verify ReadAt doesn't mutate any state. */
static void TestReaderAtConcurrent(TestingT *t) {
    (void)t;
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("0123456789"));
    SyncWaitGroup wg = {0};
    ReadAtJob jobs[5];
    for (Int i = 0; i < 5; i++) {
        jobs[i].r = &r;
        jobs[i].i = i;
        sync_wait_group_go(&wg, BURROW_FN(Func, read_at_one, &jobs[i]));
    }
    sync_wait_group_wait(&wg);
}

static void read_one(void *env) {
    Byte buf[1];
    bytes_reader_read((BytesReader *)env, slice_from(buf, 1, 1, TYPE_BYTE), NULL);
}

static void read_nil(void *env) {
    bytes_reader_read((BytesReader *)env, slice_nil(TYPE_BYTE), NULL);
}

/* Test for the race detector, to verify a Read that doesn't yield any bytes
 * is okay to use from multiple goroutines. This was our historic behavior.
 * See golang.org/issue/7856 */
static void TestEmptyReaderConcurrent(TestingT *t) {
    (void)t;
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B(""));
    SyncWaitGroup wg = {0};
    for (int i = 0; i < 5; i++) {
        sync_wait_group_go(&wg, BURROW_FN(Func, read_one, &r));
        sync_wait_group_go(&wg, BURROW_FN(Func, read_nil, &r));
    }
    sync_wait_group_wait(&wg);
}

/* buffer_test.go's test data: N bytes of the alphabet over and over. */
#define N 10000
static Byte test_bytes[N];

static void init_test_bytes(void) {
    for (Int i = 0; i < N; i++)
        test_bytes[i] = (Byte)('a' + i % 26);
}

static void TestReaderWriteTo(TestingT *t) {
    init_test_bytes();
    for (Int i = 0; i < 30; i += 3) {
        Int l = 0;
        if (i > 0)
            l = N / i;
        Str s = str_from_bytes(test_bytes, l);
        BytesReader r;
        bytes_reader_reset(&r, slice_from(test_bytes, l, l, TYPE_BYTE));
        BytesBuffer b = BYTES_BUFFER(heap_allocator());
        Error err;
        int64_t n = bytes_reader_write_to(&r, bytes_buffer_as_io_writer(&b), &err);
        if (n != (int64_t)s.len)
            testing_t_errorf_v(t, "got %v; want %v", n, s.len);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "for length %d: got error = %v; want nil", l, err);
        Str got = str_from_bytes(bytes_buffer_bytes(&b).p, bytes_buffer_len(&b));
        if (!str_eq(got, s))
            testing_t_errorf_v(t, "got string %q; want %q", got, s);
        if (bytes_reader_len(&r) != 0)
            testing_t_errorf_v(t, "reader contains %v bytes; want 0",
                               bytes_reader_len(&r));
        bytes_buffer_free(&b);
    }
}

static void TestReaderLen(TestingT *t) {
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("hello world"));
    if (bytes_reader_len(&r) != 11)
        testing_t_errorf_v(t, "r.Len(): got %d, want %d", bytes_reader_len(&r), 11);
    Byte buf[10];
    Error err;
    Int n = bytes_reader_read(&r, slice_from(buf, 10, 10, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || n != 10)
        testing_t_errorf_v(t, "Read failed: read %d %v", n, err);
    if (bytes_reader_len(&r) != 1)
        testing_t_errorf_v(t, "r.Len(): got %d, want %d", bytes_reader_len(&r), 1);
    n = bytes_reader_read(&r, slice_from(buf, 1, 1, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || n != 1)
        testing_t_errorf_v(t, "Read failed: read %d %v; want 1, nil", n, err);
    if (bytes_reader_len(&r) != 0)
        testing_t_errorf_v(t, "r.Len(): got %d, want %d", bytes_reader_len(&r), 0);
}

static void unread_after_read(BytesReader *r) {
    Byte b[1] = {0};
    bytes_reader_read(r, slice_from(b, 1, 1, TYPE_BYTE), NULL);
}

static void unread_after_read_byte(BytesReader *r) {
    bytes_reader_read_byte(r, NULL);
}

static void unread_after_unread_rune(BytesReader *r) {
    (void)bytes_reader_unread_rune(r);
}

static void unread_after_seek(BytesReader *r) {
    bytes_reader_seek(r, 0, BURROW_IO_SEEK_CURRENT, NULL);
}

static void unread_after_write_to(BytesReader *r) {
    BytesBuffer b = BYTES_BUFFER(heap_allocator());
    bytes_reader_write_to(r, bytes_buffer_as_io_writer(&b), NULL);
    bytes_buffer_free(&b);
}

static void TestUnreadRuneError(TestingT *t) {
    static const struct {
        const char *name;
        void (*f)(BytesReader *);
    } tests[] = {
        {"Read", unread_after_read},
        {"ReadByte", unread_after_read_byte},
        {"UnreadRune", unread_after_unread_rune},
        {"Seek", unread_after_seek},
        {"WriteTo", unread_after_write_to},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        BytesReader reader;
        bytes_reader_reset(&reader, BURROW_B("0123456789"));
        Error err;
        bytes_reader_read_rune(&reader, NULL, &err);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
        tests[i].f(&reader);
        if (BURROW_OK(bytes_reader_unread_rune(&reader)))
            testing_t_errorf_v(t, "Unreading after %s: expected error", tests[i].name);
    }
}

static void TestReaderDoubleUnreadRune(TestingT *t) {
    Byte data[] = "groucho";
    BytesBuffer *buf =
        bytes_new_buffer(heap_allocator(), slice_from(data, 7, 7, TYPE_BYTE));
    Error err;
    bytes_buffer_read_rune(buf, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    err = bytes_buffer_unread_byte(buf);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (BURROW_OK(bytes_buffer_unread_byte(buf)))
        testing_t_fatal_v(t, "UnreadByte: expected error, got nil");
    bytes_buffer_free(buf);
}

static Int discard_write(void *self, Slice p, Error *err) {
    (void)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static const IoWriterVT discard_vt = {NULL, discard_write};

/* io.Copy here has no WriterTo to find, so both of Go's paths are the same one,
 * through the reader and through a wrapper that hides it. */
static Int just_read(void *self, Slice p, Error *err) {
    return bytes_reader_read((BytesReader *)self, p, err);
}

static const IoReaderVT just_reader_vt = {NULL, just_read};

static void TestReaderCopyNothing(TestingT *t) {
    IoWriter discard = {&discard_vt, NULL};
    BytesReader r1, r2;
    bytes_reader_reset(&r1, slice_nil(TYPE_BYTE));
    bytes_reader_reset(&r2, slice_nil(TYPE_BYTE));
    Error with_err, without_err;
    int64_t with_n =
        io_copy(heap_allocator(), discard, bytes_reader_as_io_reader(&r1), &with_err);
    IoReader just = {&just_reader_vt, &r2};
    int64_t without_n = io_copy(heap_allocator(), discard, just, &without_err);
    if (with_n != without_n || !errors_is(with_err, without_err))
        testing_t_errorf_v(t, "behavior differs: with = %d %v; without: %d %v", with_n,
                           with_err, without_n, without_err);
}

/* tests that Len is affected by reads, but Size is not. */
static void TestReaderLenSize(TestingT *t) {
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("abc"));
    Byte one[1];
    io_read_full(bytes_reader_as_io_reader(&r), slice_from(one, 1, 1, TYPE_BYTE), NULL);
    if (bytes_reader_len(&r) != 2)
        testing_t_errorf_v(t, "Len = %d; want 2", bytes_reader_len(&r));
    if (bytes_reader_size(&r) != 3)
        testing_t_errorf_v(t, "Size = %d; want 3", bytes_reader_size(&r));
}

static void TestReaderReset(TestingT *t) {
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("世界"));
    Error err;
    bytes_reader_read_rune(&r, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadRune: unexpected error: %v", err);
    Str want = S("abcdef");
    bytes_reader_reset(&r, BURROW_B("abcdef"));
    if (BURROW_OK(bytes_reader_unread_rune(&r)))
        testing_t_errorf_v(t, "UnreadRune: expected error, got nil");
    Byte buf[16];
    Int n = io_read_full(bytes_reader_as_io_reader(&r),
                         slice_from(buf, 16, 16, TYPE_BYTE), &err);
    if (!errors_is(err, io_err_unexpected_eof))
        testing_t_errorf_v(t, "ReadAll: unexpected error: %v", err);
    Str got = str_from_bytes(buf, n);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "ReadAll: got %q, want %q", got, want);
}

static void TestReaderZero(TestingT *t) {
    BytesReader z;
    Error err;
#define ZERO() memset(&z, 0, sizeof z)
    ZERO();
    Int l = bytes_reader_len(&z);
    if (l != 0)
        testing_t_errorf_v(t, "Len: got %d, want 0", l);
    ZERO();
    Int n = bytes_reader_read(&z, slice_nil(TYPE_BYTE), &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "Read: got %d, %v; want 0, io.EOF", n, err);
    ZERO();
    n = bytes_reader_read_at(&z, slice_nil(TYPE_BYTE), 11, &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "ReadAt: got %d, %v; want 0, io.EOF", n, err);
    ZERO();
    Byte b = bytes_reader_read_byte(&z, &err);
    if (b != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "ReadByte: got %d, %v; want 0, io.EOF", b, err);
    ZERO();
    Int size;
    Rune ch = bytes_reader_read_rune(&z, &size, &err);
    if (ch != 0 || size != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "ReadRune: got %d, %d, %v; want 0, 0, io.EOF", ch, size,
                           err);
    ZERO();
    int64_t offset = bytes_reader_seek(&z, 11, BURROW_IO_SEEK_START, &err);
    if (offset != 11 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Seek: got %d, %v; want 11, nil", offset, err);
    ZERO();
    int64_t s = bytes_reader_size(&z);
    if (s != 0)
        testing_t_errorf_v(t, "Size: got %d, want 0", s);
    ZERO();
    if (BURROW_OK(bytes_reader_unread_byte(&z)))
        testing_t_errorf_v(t, "UnreadByte: got nil, want error");
    ZERO();
    if (BURROW_OK(bytes_reader_unread_rune(&z)))
        testing_t_errorf_v(t, "UnreadRune: got nil, want error");
    ZERO();
    IoWriter discard = {&discard_vt, NULL};
    int64_t wn = bytes_reader_write_to(&z, discard, &err);
    if (wn != 0 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "WriteTo: got %d, %v; want 0, nil", wn, err);
#undef ZERO
}

/* ------------------------------------------------------------ not from Go */

static void TestReaderSeeker(TestingT *t) {
    BytesReader r;
    bytes_reader_reset(&r, BURROW_B("hello"));
    IoSeeker sk = bytes_reader_as_io_seeker(&r);
    Error err;
    int64_t pos = BURROW_CALL(sk, seek, -2, BURROW_IO_SEEK_END, &err);
    CHECK(pos == 3 && BURROW_OK(err));
    Byte buf[8];
    IoReader rd = bytes_reader_as_io_reader(&r);
    Int n = BURROW_CALL(rd, read, slice_from(buf, 8, 8, TYPE_BYTE), &err);
    CHECK(n == 2 && str_eq(str_from_bytes(buf, n), S("lo")));
    bytes_reader_seek(&r, 0, 7, &err);
    CHECK(str_eq(error_text(err), S("bytes.Reader.Seek: invalid whence")));
    CHECK(rd.vt->self_type == TYPE_BYTES_READER);
}

static void TestNewReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    BytesReader *r = bytes_new_reader(arena_allocator(&ar), BURROW_B("xy"));
    CHECK(r != NULL);
    CHECK(bytes_reader_len(r) == 2);
    Error err;
    CHECK(bytes_reader_read_byte(r, &err) == 'x');
    CHECK(str_eq(error_text(bytes_reader_unread_rune(r)),
                 S("bytes.Reader.UnreadRune: previous operation was not ReadRune")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReader)                                                                      \
    X(TestReadAfterBigSeek)                                                            \
    X(TestReaderAt)                                                                    \
    X(TestReaderAtConcurrent)                                                          \
    X(TestEmptyReaderConcurrent)                                                       \
    X(TestReaderWriteTo)                                                               \
    X(TestReaderLen)                                                                   \
    X(TestUnreadRuneError)                                                             \
    X(TestReaderDoubleUnreadRune)                                                      \
    X(TestReaderCopyNothing)                                                           \
    X(TestReaderLenSize)                                                               \
    X(TestReaderReset)                                                                 \
    X(TestReaderZero)                                                                  \
    X(TestReaderSeeker)                                                                \
    X(TestNewReader)

TESTING_MAIN(TESTS)
