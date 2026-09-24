/* Derived from Go's src/strings/reader_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
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
    StringsReader r;
    strings_reader_reset(&r, S("0123456789"));
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
         "strings.Reader.Seek: negative position"},
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
        int64_t pos = strings_reader_seek(&r, tests[i].off, tests[i].seek, &err);
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
        Int n = strings_reader_read(
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
    StringsReader r;
    strings_reader_reset(&r, S("0123456789"));
    Error err;
    strings_reader_seek(&r, ((int64_t)1 << 31) + 5, BURROW_IO_SEEK_START, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Byte buf[10];
    Int n = strings_reader_read(&r, slice_from(buf, 10, 10, TYPE_BYTE), &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "Read = %d, %v; want 0, EOF", n, err);
}

static void TestReaderAt(TestingT *t) {
    StringsReader r;
    strings_reader_reset(&r, S("0123456789"));
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
        {-1, 0, SI(""), false, "strings.Reader.ReadAt: negative offset"},
    };
    Byte b[16];
    for (Int i = 0; i < LEN(tests); i++) {
        Error err;
        Int rn = strings_reader_read_at(
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
    StringsReader *r;
    int64_t i;
} ReadAtJob;

static void read_at_one(void *env) {
    ReadAtJob *j = (ReadAtJob *)env;
    Byte buf[1];
    strings_reader_read_at(j->r, slice_from(buf, 1, 1, TYPE_BYTE), j->i, NULL);
}

/* Test for the race detector, to verify ReadAt doesn't mutate any state. */
static void TestReaderAtConcurrent(TestingT *t) {
    (void)t;
    StringsReader r;
    strings_reader_reset(&r, S("0123456789"));
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
    strings_reader_read((StringsReader *)env, slice_from(buf, 1, 1, TYPE_BYTE), NULL);
}

static void read_nil(void *env) {
    strings_reader_read((StringsReader *)env, slice_nil(TYPE_BYTE), NULL);
}

/* Test for the race detector, to verify a Read that doesn't yield any bytes
 * is okay to use from multiple goroutines. This was our historic behavior.
 * See golang.org/issue/7856 */
static void TestEmptyReaderConcurrent(TestingT *t) {
    (void)t;
    StringsReader r;
    strings_reader_reset(&r, S(""));
    SyncWaitGroup wg = {0};
    for (int i = 0; i < 5; i++) {
        sync_wait_group_go(&wg, BURROW_FN(Func, read_one, &r));
        sync_wait_group_go(&wg, BURROW_FN(Func, read_nil, &r));
    }
    sync_wait_group_wait(&wg);
}

static void TestWriteTo(TestingT *t) {
    Str str = S("0123456789");
    for (Int i = 0; i <= str.len; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Str s = str_from_bytes(str.p + i, str.len - i);
        StringsReader r;
        strings_reader_reset(&r, s);
        StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
        Error err;
        int64_t n = strings_reader_write_to(&r, strings_builder_as_io_writer(&b), &err);
        if (n != (int64_t)s.len)
            testing_t_errorf_v(t, "got %v; want %v", n, s.len);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "for length %d: got error = %v; want nil", s.len,
                               err);
        if (!str_eq(strings_builder_string(&b), s))
            testing_t_errorf_v(t, "got string %q; want %q", strings_builder_string(&b),
                               s);
        if (strings_reader_len(&r) != 0)
            testing_t_errorf_v(t, "reader contains %v bytes; want 0",
                               strings_reader_len(&r));
        arena_free(&ar);
    }
}

/* tests that Len is affected by reads, but Size is not. */
static void TestReaderLenSize(TestingT *t) {
    StringsReader r;
    strings_reader_reset(&r, S("abc"));
    Byte one[1];
    io_read_full(strings_reader_as_io_reader(&r), slice_from(one, 1, 1, TYPE_BYTE),
                 NULL);
    if (strings_reader_len(&r) != 2)
        testing_t_errorf_v(t, "Len = %d; want 2", strings_reader_len(&r));
    if (strings_reader_size(&r) != 3)
        testing_t_errorf_v(t, "Size = %d; want 3", strings_reader_size(&r));
}

static void TestReaderReset(TestingT *t) {
    StringsReader r;
    strings_reader_reset(&r, S("世界"));
    Error err;
    strings_reader_read_rune(&r, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadRune: unexpected error: %v", err);
    Str want = S("abcdef");
    strings_reader_reset(&r, want);
    if (BURROW_OK(strings_reader_unread_rune(&r)))
        testing_t_errorf_v(t, "UnreadRune: expected error, got nil");
    Byte buf[16];
    Int n = io_read_full(strings_reader_as_io_reader(&r),
                         slice_from(buf, 16, 16, TYPE_BYTE), &err);
    if (!errors_is(err, io_err_unexpected_eof))
        testing_t_errorf_v(t, "ReadAll: unexpected error: %v", err);
    Str got = str_from_bytes(buf, n);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "ReadAll: got %q, want %q", got, want);
}

static Int discard_write(void *self, Slice p, Error *err) {
    (void)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static const IoWriterVT discard_vt = {NULL, discard_write};

static void TestReaderZero(TestingT *t) {
    StringsReader z;
    Error err;
#define ZERO() memset(&z, 0, sizeof z)
    ZERO();
    Int l = strings_reader_len(&z);
    if (l != 0)
        testing_t_errorf_v(t, "Len: got %d, want 0", l);
    ZERO();
    Int n = strings_reader_read(&z, slice_nil(TYPE_BYTE), &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "Read: got %d, %v; want 0, io.EOF", n, err);
    ZERO();
    n = strings_reader_read_at(&z, slice_nil(TYPE_BYTE), 11, &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "ReadAt: got %d, %v; want 0, io.EOF", n, err);
    ZERO();
    Byte b = strings_reader_read_byte(&z, &err);
    if (b != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "ReadByte: got %d, %v; want 0, io.EOF", b, err);
    ZERO();
    Int size;
    Rune ch = strings_reader_read_rune(&z, &size, &err);
    if (ch != 0 || size != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "ReadRune: got %d, %d, %v; want 0, 0, io.EOF", ch, size,
                           err);
    ZERO();
    int64_t offset = strings_reader_seek(&z, 11, BURROW_IO_SEEK_START, &err);
    if (offset != 11 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Seek: got %d, %v; want 11, nil", offset, err);
    ZERO();
    int64_t s = strings_reader_size(&z);
    if (s != 0)
        testing_t_errorf_v(t, "Size: got %d, want 0", s);
    ZERO();
    if (BURROW_OK(strings_reader_unread_byte(&z)))
        testing_t_errorf_v(t, "UnreadByte: got nil, want error");
    ZERO();
    if (BURROW_OK(strings_reader_unread_rune(&z)))
        testing_t_errorf_v(t, "UnreadRune: got nil, want error");
    ZERO();
    IoWriter discard = {&discard_vt, NULL};
    int64_t wn = strings_reader_write_to(&z, discard, &err);
    if (wn != 0 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "WriteTo: got %d, %v; want 0, nil", wn, err);
#undef ZERO
}

/* ------------------------------------------------------------ not from Go */

static void TestReaderSeeker(TestingT *t) {
    StringsReader r;
    strings_reader_reset(&r, S("hello"));
    IoSeeker sk = strings_reader_as_io_seeker(&r);
    Error err;
    int64_t pos = BURROW_CALL(sk, seek, -2, BURROW_IO_SEEK_END, &err);
    CHECK(pos == 3 && BURROW_OK(err));
    Byte buf[8];
    IoReader rd = strings_reader_as_io_reader(&r);
    Int n = BURROW_CALL(rd, read, slice_from(buf, 8, 8, TYPE_BYTE), &err);
    CHECK(n == 2 && str_eq(str_from_bytes(buf, n), S("lo")));
    strings_reader_seek(&r, 0, 7, &err);
    CHECK(str_eq(error_text(err), S("strings.Reader.Seek: invalid whence")));
    CHECK(rd.vt->self_type == TYPE_STRINGS_READER);
}

static void TestNewReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsReader *r = strings_new_reader(arena_allocator(&ar), S("xy"));
    CHECK(r != NULL);
    CHECK(strings_reader_len(r) == 2);
    Error err;
    CHECK(strings_reader_read_byte(r, &err) == 'x');
    CHECK(str_eq(error_text(strings_reader_unread_rune(r)),
                 S("strings.Reader.UnreadRune: previous operation was not ReadRune")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReader)                                                                      \
    X(TestReadAfterBigSeek)                                                            \
    X(TestReaderAt)                                                                    \
    X(TestReaderAtConcurrent)                                                          \
    X(TestEmptyReaderConcurrent)                                                       \
    X(TestWriteTo)                                                                     \
    X(TestReaderLenSize)                                                               \
    X(TestReaderReset)                                                                 \
    X(TestReaderZero)                                                                  \
    X(TestReaderSeeker)                                                                \
    X(TestNewReader)

TESTING_MAIN(TESTS)
