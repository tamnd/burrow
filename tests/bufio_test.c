/* Derived from Go's src/bufio/bufio_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bufio.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include "check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

enum { MIN_READ_BUFFER_SIZE = 16, DEFAULT_BUF_SIZE = 4096 };

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

static Alloc *heap(void) {
    return heap_allocator();
}

/* Errors the tests fail with, made the way Go's errors.New would be but with
 * nothing to free. */
#define TEST_ERROR(name, text)                                                         \
    static const Str name##__text = {(const Byte *)(text), (Int)(sizeof(text) - 1)};   \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

TEST_ERROR(err_timeout, "timeout");
TEST_ERROR(err_fake, "fake error");
TEST_ERROR(err_five_then_error, "5-then-error");
TEST_ERROR(err_read_from_error, "writerWithReadFromError error");
TEST_ERROR(err_write_only_error, "writeErrorOnlyWriter error");

static const Type test_type = {
    {(const Byte *)"testType", 8},
    {(const Byte *)"bufio_test", 10},
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
    0x62746573U,
    NULL,
};

static bool same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

static bool is_eof(Error e) {
    return same_error(e, io_eof);
}

static Str text_of(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static Slice bytes_of(void *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static Slice str_bytes(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static StringsReader *new_strings_reader(Str s) {
    StringsReader *r =
        (StringsReader *)mem_alloc(a, sizeof *r, _Alignof(StringsReader));
    strings_reader_reset(r, s);
    return r;
}

static IoReader strings_io(Str s) {
    return strings_reader_as_io_reader(new_strings_reader(s));
}

static IoReader bytes_io(Slice b) {
    return bytes_reader_as_io_reader(bytes_new_reader(a, b));
}

/* The panic text of f(env), or an empty string when it did not panic. */
static char panic_buf[256];

static bool panics(void (*f)(void *), void *env, Str *msg) {
    volatile bool got = false;
    volatile Int n = 0;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        n = s.len < (Int)sizeof panic_buf ? s.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, s.p, (size_t)n);
        got = true;
    }
    BURROW_TRY_END;
    if (msg != NULL)
        *msg = str_from_bytes((const Byte *)panic_buf, n);
    return got;
}

/* ------------------------------------------------------ testing/iotest bits
 *
 * The four readers bufio's tests borrow from testing/iotest, and its
 * ErrTimeout. */

typedef struct Wrap {
    IoReader r;
} Wrap;

static Wrap *wrap(IoReader r) {
    Wrap *w = (Wrap *)mem_alloc(a, sizeof *w, _Alignof(Wrap));
    w->r = r;
    return w;
}

/* OneByteReader reads one byte at a time. */
static Int one_byte_read(void *self, Slice p, Error *err) {
    if (p.len == 0)
        return 0;
    return BURROW_CALL(((Wrap *)self)->r, read, bytes_of(p.p, 1), err);
}

static const IoReaderVT one_byte_vt = {&test_type, one_byte_read};

static IoReader one_byte_reader(IoReader r) {
    IoReader out = {&one_byte_vt, wrap(r)};
    return out;
}

/* HalfReader reads half as many bytes as asked for. */
static Int half_read(void *self, Slice p, Error *err) {
    return BURROW_CALL(((Wrap *)self)->r, read, bytes_of(p.p, (p.len + 1) / 2), err);
}

static const IoReaderVT half_vt = {&test_type, half_read};

static IoReader half_reader(IoReader r) {
    IoReader out = {&half_vt, wrap(r)};
    return out;
}

/* DataErrReader returns the final error with the last data read, rather than
 * by itself with zero bytes. */
typedef struct DataErr {
    IoReader r;
    Byte data[1024];
    Int off, len; /* the unread part of data */
} DataErr;

static Int data_err_read(void *self, Slice p, Error *err) {
    DataErr *r = (DataErr *)self;
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    /* loop because first call needs two reads: one to get data and a second to
     * look for an error. */
    for (;;) {
        if (r->len == 0) {
            Error e1 = BURROW_NO_ERROR;
            Int n1 = BURROW_CALL(r->r, read, bytes_of(r->data, LEN(r->data)), &e1);
            r->off = 0;
            r->len = n1;
            e = e1;
        }
        if (n > 0 || BURROW_FAILED(e))
            break;
        n = r->len < p.len ? r->len : p.len;
        if (n > 0)
            memcpy(p.p, r->data + r->off, (size_t)n);
        r->off += n;
        r->len -= n;
    }
    *err = e;
    return n;
}

static const IoReaderVT data_err_vt = {&test_type, data_err_read};

static IoReader data_err_reader(IoReader r) {
    DataErr *d = (DataErr *)mem_alloc(a, sizeof *d, _Alignof(DataErr));
    d->r = r;
    IoReader out = {&data_err_vt, d};
    return out;
}

/* TimeoutReader returns ErrTimeout on the second read with no data.
 * Subsequent calls to read succeed. */
typedef struct Timeout {
    IoReader r;
    int count;
} Timeout;

static Int timeout_read(void *self, Slice p, Error *err) {
    Timeout *r = (Timeout *)self;
    r->count++;
    if (r->count == 2) {
        *err = err_timeout;
        return 0;
    }
    return BURROW_CALL(r->r, read, p, err);
}

static const IoReaderVT timeout_vt = {&test_type, timeout_read};

static IoReader timeout_reader(IoReader r) {
    Timeout *t = (Timeout *)mem_alloc(a, sizeof *t, _Alignof(Timeout));
    t->r = r;
    IoReader out = {&timeout_vt, t};
    return out;
}

static IoReader full_reader(IoReader r) {
    return r;
}

/* ------------------------------------------------------------------ Reader */

/* Reads from a reader and rot13s the result. */
static Int rot13_read(void *self, Slice p, Error *err) {
    Int n = BURROW_CALL(((Wrap *)self)->r, read, p, err);
    Byte *q = (Byte *)p.p;
    for (Int i = 0; i < n; i++) {
        Byte c = q[i] | 0x20; /* lowercase byte */
        if ('a' <= c && c <= 'm')
            q[i] = (Byte)(q[i] + 13);
        else if ('n' <= c && c <= 'z')
            q[i] = (Byte)(q[i] - 13);
    }
    return n;
}

static const IoReaderVT rot13_vt = {&test_type, rot13_read};

/* Call ReadByte to accumulate the text of a file. The result is in out. */
static Str read_bytes(BufioReader *buf, Byte *out) {
    Int nb = 0;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Byte c = bufio_reader_read_byte(buf, &err);
        if (is_eof(err))
            break;
        if (BURROW_OK(err))
            out[nb++] = c;
        else if (!same_error(err, err_timeout))
            panic_str(S("Data: read error"));
    }
    return str_from_bytes(out, nb);
}

static void TestReaderSimple(TestingT *t) {
    Byte out[1000];
    Str data = S("hello world");
    BufioReader *b = bufio_new_reader(heap(), strings_io(data));
    Str s = read_bytes(b, out);
    if (!str_eq(s, S("hello world")))
        testing_t_errorf_v(t, "simple hello world test failed: got %q", s);
    bufio_reader_free(b);

    IoReader rot = {&rot13_vt, wrap(strings_io(data))};
    b = bufio_new_reader(heap(), rot);
    s = read_bytes(b, out);
    if (!str_eq(s, S("uryyb jbeyq")))
        testing_t_errorf_v(t, "rot13 hello world test failed: got %q", s);
    bufio_reader_free(b);
}

typedef struct ReadMaker {
    const char *name;
    IoReader (*fn)(IoReader);
} ReadMaker;

static const ReadMaker readMakers[] = {
    {"full", full_reader},         {"byte", one_byte_reader},   {"half", half_reader},
    {"data+err", data_err_reader}, {"timeout", timeout_reader},
};

/* Call ReadString (which ends up calling everything else) to accumulate the
 * text of a file. */
static Str read_lines(BufioReader *b, Byte *out) {
    Int n = 0;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Str s1 = bufio_reader_read_string(b, heap(), '\n', &err);
        if (is_eof(err)) {
            if (s1.len > 0)
                mem_free(heap(), (void *)(uintptr_t)s1.p, (size_t)s1.len, 1);
            break;
        }
        if (BURROW_FAILED(err) && !same_error(err, err_timeout))
            panic_str(S("GetLines: read error"));
        if (s1.len > 0)
            memcpy(out + n, s1.p, (size_t)s1.len);
        n += s1.len;
        if (s1.len > 0)
            mem_free(heap(), (void *)(uintptr_t)s1.p, (size_t)s1.len, 1);
    }
    return str_from_bytes(out, n);
}

/* Call Read to accumulate the text of a file. */
static Str reads(BufioReader *buf, Int m, Byte *b) {
    Int nb = 0;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = bufio_reader_read(buf, bytes_of(b + nb, m), &err);
        nb += n;
        if (is_eof(err))
            break;
    }
    return str_from_bytes(b, nb);
}

static Str reads1(BufioReader *b, Byte *out) {
    return reads(b, 1, out);
}
static Str reads2(BufioReader *b, Byte *out) {
    return reads(b, 2, out);
}
static Str reads3(BufioReader *b, Byte *out) {
    return reads(b, 3, out);
}
static Str reads4(BufioReader *b, Byte *out) {
    return reads(b, 4, out);
}
static Str reads5(BufioReader *b, Byte *out) {
    return reads(b, 5, out);
}
static Str reads7(BufioReader *b, Byte *out) {
    return reads(b, 7, out);
}

typedef struct BufReaderCase {
    const char *name;
    Str (*fn)(BufioReader *, Byte *);
} BufReaderCase;

static const BufReaderCase bufreaders[] = {
    {"1", reads1}, {"2", reads2}, {"3", reads3},         {"4", reads4},
    {"5", reads5}, {"7", reads7}, {"bytes", read_bytes}, {"lines", read_lines},
};

static const Int bufsizes[] = {
    0, MIN_READ_BUFFER_SIZE, 23, 32, 46, 64, 93, 128, 1024, 4096,
};

static void TestReader(TestingT *t) {
    static char texts[31][600];
    char str[40] = "";
    static char all[600];
    Int slen = 0;
    size_t alen = 0;
    for (Int i = 0; i < LEN(texts) - 1; i++) {
        snprintf(texts[i], sizeof texts[i], "%s\n", str);
        memcpy(all + alen, texts[i], strlen(texts[i]));
        alen += strlen(texts[i]);
        str[slen++] = (char)(i % 26 + 'a');
        str[slen] = 0;
    }
    all[alen] = 0;
    memcpy(texts[LEN(texts) - 1], all, alen + 1);

    static Byte out[1000];
    for (Int h = 0; h < LEN(texts); h++) {
        Str text = str_from_cstr(texts[h]);
        for (Int i = 0; i < LEN(readMakers); i++) {
            for (Int j = 0; j < LEN(bufreaders); j++) {
                for (Int k = 0; k < LEN(bufsizes); k++) {
                    const ReadMaker *readmaker = &readMakers[i];
                    const BufReaderCase *bufreader = &bufreaders[j];
                    Int bufsize = bufsizes[k];
                    StringsReader sr;
                    strings_reader_reset(&sr, text);
                    IoReader read = readmaker->fn(strings_reader_as_io_reader(&sr));
                    BufioReader *buf = bufio_new_reader_size(heap(), read, bufsize);
                    Str s = bufreader->fn(buf, out);
                    if (!str_eq(s, text))
                        testing_t_errorf_v(
                            t, "reader=%s fn=%s bufsize=%d want=%q got=%q",
                            readmaker->name, bufreader->name, bufsize, text, s);
                    bufio_reader_free(buf);
                }
            }
        }
    }
}

static Int zero_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    (void)err;
    return 0;
}

static const IoReaderVT zero_vt = {&test_type, zero_read};

/* Go runs the ReadByte in a goroutine and gives it a second. Here it is called
 * directly: if it looped forever the test binary's own timeout would say so. */
static void TestZeroReader(TestingT *t) {
    IoReader z = {&zero_vt, NULL};
    BufioReader *r = bufio_new_reader(heap(), z);
    Error err = BURROW_NO_ERROR;
    (void)bufio_reader_read_byte(r, &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "error expected");
    else if (!same_error(err, io_err_no_progress))
        testing_t_error_v(t, "unexpected error:", err);
    bufio_reader_free(r);
}

/* A StringReader delivers its data one string segment at a time via Read. */
typedef struct SegReader {
    const char *const *data;
    Int n;
    Int step;
} SegReader;

static Int seg_read(void *self, Slice p, Error *err) {
    SegReader *r = (SegReader *)self;
    if (r->step < r->n) {
        const char *s = r->data[r->step];
        Int n = (Int)strlen(s);
        if (n > p.len)
            n = p.len;
        if (n > 0)
            memcpy(p.p, s, (size_t)n);
        r->step++;
        return n;
    }
    *err = io_eof;
    return 0;
}

static const IoReaderVT seg_vt = {&test_type, seg_read};

static IoReader seg_reader(SegReader *r, const char *const *data, Int n) {
    r->data = data;
    r->n = n;
    r->step = 0;
    IoReader out = {&seg_vt, r};
    return out;
}

static Str join(const char *const *segments, Int n, char *out) {
    size_t len = 0;
    for (Int i = 0; i < n; i++) {
        size_t k = strlen(segments[i]);
        memcpy(out + len, segments[i], k);
        len += k;
    }
    out[len] = 0;
    return str_from_bytes((const Byte *)out, (Int)len);
}

static void read_rune_segments(TestingT *t, const char *const *segments, Int nseg) {
    char want_buf[64], got_buf[64];
    Str want = join(segments, nseg, want_buf);
    Int got_len = 0;
    SegReader sr;
    BufioReader *r = bufio_new_reader(heap(), seg_reader(&sr, segments, nseg));
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Rune c = bufio_reader_read_rune(r, NULL, &err);
        if (BURROW_FAILED(err)) {
            if (!is_eof(err)) {
                bufio_reader_free(r);
                return;
            }
            break;
        }
        got_len += utf8_encode_rune(bytes_of(got_buf + got_len, UTF8_UTF_MAX), c);
    }
    Str got = str_from_bytes((const Byte *)got_buf, got_len);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "segments=%d got=%s want=%s", nseg, got, want);
    bufio_reader_free(r);
}

static const char *const seg0[] = {""};
static const char *const seg1[] = {""};
static const char *const seg2[] = {"\xe6\x97\xa5", "\xe6\x9c\xac\xe8\xaa\x9e"};
static const char *const seg3[] = {"\xe6\x97\xa5", "\xe6\x9c\xac", "\xe8\xaa\x9e"};
static const char *const seg4[] = {"\xe6", "\x97\xa5\xe6", "\x9c\xac\xe8\xaa\x9e"};
static const char *const seg5[] = {"Hello", ", ", "World", "!"};
static const char *const seg6[] = {"Hello", ", ", "", "World", "!"};

typedef struct SegCase {
    const char *const *s;
    Int n;
} SegCase;

static const SegCase segmentList[] = {
    {seg0, 0}, {seg1, 1}, {seg2, 2}, {seg3, 3},
    {seg3, 3}, {seg4, 3}, {seg5, 4}, {seg6, 5},
};

static void TestReadRune(TestingT *t) {
    for (Int i = 0; i < LEN(segmentList); i++)
        read_rune_segments(t, segmentList[i].s, segmentList[i].n);
}

static void TestUnreadRune(TestingT *t) {
    static const char *const segments[] = {"Hello, world:",
                                           "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"};
    char want_buf[64], got_buf[64];
    Str want = join(segments, 2, want_buf);
    Int got_len = 0;
    SegReader sr;
    BufioReader *r = bufio_new_reader(a, seg_reader(&sr, segments, 2));
    /* Normal execution. */
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Rune r1 = bufio_reader_read_rune(r, NULL, &err);
        if (BURROW_FAILED(err)) {
            if (!is_eof(err))
                testing_t_error_v(t, "unexpected error on ReadRune:", err);
            break;
        }
        got_len += utf8_encode_rune(bytes_of(got_buf + got_len, UTF8_UTF_MAX), r1);
        /* Put it back and read it again. */
        err = bufio_reader_unread_rune(r);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, "unexpected error on UnreadRune:", err);
        Rune r2 = bufio_reader_read_rune(r, NULL, &err);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, "unexpected error reading after unreading:", err);
        if (r1 != r2)
            testing_t_fatalf_v(t, "incorrect rune after unread: got %c, want %c", r1,
                               r2);
    }
    Str got = str_from_bytes((const Byte *)got_buf, got_len);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
}

static void TestNoUnreadRuneAfterPeek(TestingT *t) {
    BufioReader *br = bufio_new_reader(a, strings_io(S("example")));
    (void)bufio_reader_read_rune(br, NULL, NULL);
    (void)bufio_reader_peek(br, 1, NULL);
    if (BURROW_OK(bufio_reader_unread_rune(br)))
        testing_t_error_v(t, "UnreadRune didn't fail after Peek");
}

static void TestNoUnreadByteAfterPeek(TestingT *t) {
    BufioReader *br = bufio_new_reader(a, strings_io(S("example")));
    (void)bufio_reader_read_byte(br, NULL);
    (void)bufio_reader_peek(br, 1, NULL);
    if (BURROW_OK(bufio_reader_unread_byte(br)))
        testing_t_error_v(t, "UnreadByte didn't fail after Peek");
}

static void TestNoUnreadRuneAfterDiscard(TestingT *t) {
    BufioReader *br = bufio_new_reader(a, strings_io(S("example")));
    (void)bufio_reader_read_rune(br, NULL, NULL);
    (void)bufio_reader_discard(br, 1, NULL);
    if (BURROW_OK(bufio_reader_unread_rune(br)))
        testing_t_error_v(t, "UnreadRune didn't fail after Discard");
}

static void TestNoUnreadByteAfterDiscard(TestingT *t) {
    BufioReader *br = bufio_new_reader(a, strings_io(S("example")));
    (void)bufio_reader_read_byte(br, NULL);
    (void)bufio_reader_discard(br, 1, NULL);
    if (BURROW_OK(bufio_reader_unread_byte(br)))
        testing_t_error_v(t, "UnreadByte didn't fail after Discard");
}

static void TestNoUnreadRuneAfterWriteTo(TestingT *t) {
    BufioReader *br = bufio_new_reader(a, strings_io(S("example")));
    (void)bufio_reader_write_to(br, io_discard, NULL);
    if (BURROW_OK(bufio_reader_unread_rune(br)))
        testing_t_error_v(t, "UnreadRune didn't fail after WriteTo");
}

static void TestNoUnreadByteAfterWriteTo(TestingT *t) {
    BufioReader *br = bufio_new_reader(a, strings_io(S("example")));
    (void)bufio_reader_write_to(br, io_discard, NULL);
    if (BURROW_OK(bufio_reader_unread_byte(br)))
        testing_t_error_v(t, "UnreadByte didn't fail after WriteTo");
}

static void TestUnreadByte(TestingT *t) {
    static const char *const segments[] = {"Hello, ", "world"};
    char want_buf[64], got_buf[64];
    Str want = join(segments, 2, want_buf);
    Int got_len = 0;
    SegReader sr;
    BufioReader *r = bufio_new_reader(a, seg_reader(&sr, segments, 2));
    /* Normal execution. */
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Byte b1 = bufio_reader_read_byte(r, &err);
        if (BURROW_FAILED(err)) {
            if (!is_eof(err))
                testing_t_error_v(t, "unexpected error on ReadByte:", err);
            break;
        }
        got_buf[got_len++] = (char)b1;
        /* Put it back and read it again. */
        err = bufio_reader_unread_byte(r);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, "unexpected error on UnreadByte:", err);
        Byte b2 = bufio_reader_read_byte(r, &err);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, "unexpected error reading after unreading:", err);
        if (b1 != b2)
            testing_t_fatalf_v(t, "incorrect byte after unread: got %q, want %q",
                               (Rune)b1, (Rune)b2);
    }
    Str got = str_from_bytes((const Byte *)got_buf, got_len);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
}

static void TestUnreadByteMultiple(TestingT *t) {
    static const char *const segments[] = {"Hello, ", "world"};
    char data_buf[64];
    Str data = join(segments, 2, data_buf);
    for (Int n = 0; n <= data.len; n++) {
        SegReader sr;
        BufioReader *r = bufio_new_reader(a, seg_reader(&sr, segments, 2));
        /* Read n bytes. */
        for (Int i = 0; i < n; i++) {
            Error err = BURROW_NO_ERROR;
            Byte b = bufio_reader_read_byte(r, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "n = %d: unexpected error on ReadByte: %v", n,
                                   err);
            if (b != data.p[i])
                testing_t_fatalf_v(
                    t, "n = %d: incorrect byte returned from ReadByte: got %q, want %q",
                    n, (Rune)b, (Rune)data.p[i]);
        }
        /* Unread one byte if there is one. */
        if (n > 0) {
            Error err = bufio_reader_unread_byte(r);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "n = %d: unexpected error on UnreadByte: %v", n,
                                   err);
        }
        /* Test that we cannot unread any further. */
        if (BURROW_OK(bufio_reader_unread_byte(r)))
            testing_t_errorf_v(t, "n = %d: expected error on UnreadByte", n);
    }
}

static Slice read_with_bytes(BufioReader *r, Byte delim, Error *err) {
    return bufio_reader_read_bytes(r, a, delim, err);
}

static Slice read_with_slice(BufioReader *r, Byte delim, Error *err) {
    return bufio_reader_read_slice(r, delim, err);
}

static Slice read_with_string(BufioReader *r, Byte delim, Error *err) {
    Str data = bufio_reader_read_string(r, a, delim, err);
    return str_bytes(data);
}

static void read_to(TestingT *t, Int rno, BufioReader *r,
                    Slice (*read)(BufioReader *, Byte, Error *), Byte delim,
                    const char *want) {
    Error err = BURROW_NO_ERROR;
    Slice data = read(r, delim, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "#%d: unexpected error reading to %c: %v", rno,
                           (Rune)delim, err);
    Str got = text_of(data);
    if (!str_eq(got, str_from_cstr(want)))
        testing_t_fatalf_v(t, "#%d: got %q, want %q", rno, got, want);
}

static void TestUnreadByteOthers(TestingT *t) {
    /* A list of readers to use in conjunction with UnreadByte. ReadLine
     * doesn't fit the data/pattern easily so we leave it out. It should be
     * covered via the ReadSlice test since ReadLine simply calls ReadSlice,
     * and it's that function that handles the last byte. */
    Slice (*readers[])(BufioReader *, Byte, Error *) = {
        read_with_bytes,
        read_with_slice,
        read_with_string,
    };

    /* Try all readers with UnreadByte. */
    for (Int rno = 0; rno < LEN(readers); rno++) {
        /* Some input data that is longer than the minimum reader buffer size. */
        enum { N = 10 };
        BytesBuffer buf = BYTES_BUFFER(a);
        for (int i = 0; i < N; i++)
            (void)bytes_buffer_write_string(&buf, S("abcdefg"), NULL);

        BufioReader *r = bufio_new_reader_size(a, bytes_buffer_as_io_reader(&buf),
                                               MIN_READ_BUFFER_SIZE);

        /* Read the data with occasional UnreadByte calls. */
        for (int i = 0; i < N; i++) {
            read_to(t, rno, r, readers[rno], 'd', "abcd");
            for (int j = 0; j < 3; j++) {
                Error err = bufio_reader_unread_byte(r);
                if (BURROW_FAILED(err))
                    testing_t_fatalf_v(t, "#%d: unexpected error on UnreadByte: %v",
                                       rno, err);
                read_to(t, rno, r, readers[rno], 'd', "d");
            }
            read_to(t, rno, r, readers[rno], 'g', "efg");
        }

        /* All data should have been read. */
        Error err = BURROW_NO_ERROR;
        (void)bufio_reader_read_byte(r, &err);
        if (!is_eof(err))
            testing_t_errorf_v(t, "#%d: got error %v; want EOF", rno, err);
    }
}

/* Test that UnreadRune fails if the preceding operation was not a ReadRune. */
static void TestUnreadRuneError(TestingT *t) {
    Byte buf[3]; /* All runes in this test are 3 bytes long */
    static const char *const data[] = {"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
                                       "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
                                       "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"};
    SegReader sr;
    BufioReader *r = bufio_new_reader(a, seg_reader(&sr, data, 1));
    if (BURROW_OK(bufio_reader_unread_rune(r)))
        testing_t_error_v(t, "expected error on UnreadRune from fresh buffer");
    Error err = BURROW_NO_ERROR;
    (void)bufio_reader_read_rune(r, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on ReadRune (1):", err);
    if (BURROW_FAILED(err = bufio_reader_unread_rune(r)))
        testing_t_error_v(t, "unexpected error on UnreadRune (1):", err);
    if (BURROW_OK(bufio_reader_unread_rune(r)))
        testing_t_error_v(t, "expected error after UnreadRune (1)");
    /* Test error after Read. */
    (void)bufio_reader_read_rune(r, NULL, &err); /* reset state */
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on ReadRune (2):", err);
    (void)bufio_reader_read(r, bytes_of(buf, 3), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on Read (2):", err);
    if (BURROW_OK(bufio_reader_unread_rune(r)))
        testing_t_error_v(t, "expected error after Read (2)");
    /* Test error after ReadByte. */
    (void)bufio_reader_read_rune(r, NULL, &err); /* reset state */
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on ReadRune (2):", err);
    for (int i = 0; i < 3; i++) {
        (void)bufio_reader_read_byte(r, &err);
        if (BURROW_FAILED(err))
            testing_t_error_v(t, "unexpected error on ReadByte (2):", err);
    }
    if (BURROW_OK(bufio_reader_unread_rune(r)))
        testing_t_error_v(t, "expected error after ReadByte");
    /* Test error after UnreadByte. */
    (void)bufio_reader_read_rune(r, NULL, &err); /* reset state */
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on ReadRune (3):", err);
    (void)bufio_reader_read_byte(r, &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on ReadByte (3):", err);
    err = bufio_reader_unread_byte(r);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on UnreadByte (3):", err);
    if (BURROW_OK(bufio_reader_unread_rune(r)))
        testing_t_error_v(t, "expected error after UnreadByte (3)");
    /* Test error after ReadSlice. */
    (void)bufio_reader_read_rune(r, NULL, &err); /* reset state */
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "unexpected error on ReadRune (4):", err);
    (void)bufio_reader_read_slice(r, 0, &err);
    if (!is_eof(err))
        testing_t_error_v(t, "unexpected error on ReadSlice (4):", err);
    if (BURROW_OK(bufio_reader_unread_rune(r)))
        testing_t_error_v(t, "expected error after ReadSlice (4)");
}

static void TestUnreadRuneAtEOF(TestingT *t) {
    /* UnreadRune/ReadRune should error at EOF (was a bug; used to panic) */
    BufioReader *r = bufio_new_reader(a, strings_io(S("x")));
    (void)bufio_reader_read_rune(r, NULL, NULL);
    (void)bufio_reader_read_rune(r, NULL, NULL);
    (void)bufio_reader_unread_rune(r);
    Error err = BURROW_NO_ERROR;
    (void)bufio_reader_read_rune(r, NULL, &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error at EOF");
    else if (!is_eof(err))
        testing_t_error_v(t, "expected EOF; got", err);
}

static void TestReadWriteRune(TestingT *t) {
    enum { NRune = 1000 };
    BytesBuffer byte_buf = BYTES_BUFFER(a);
    BufioWriter *w = bufio_new_writer(a, bytes_buffer_as_io_writer(&byte_buf));
    /* Write the runes out using WriteRune */
    Byte buf[UTF8_UTF_MAX];
    for (Rune r = 0; r < NRune; r++) {
        Int size = utf8_encode_rune(bytes_of(buf, UTF8_UTF_MAX), r);
        Error err = BURROW_NO_ERROR;
        Int nbytes = bufio_writer_write_rune(w, r, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "WriteRune(0x%x) error: %s", r, err);
        if (nbytes != size)
            testing_t_fatalf_v(t, "WriteRune(0x%x) expected %d, got %d", r, size,
                               nbytes);
    }
    (void)bufio_writer_flush(w);

    BufioReader *r = bufio_new_reader(a, bytes_buffer_as_io_reader(&byte_buf));
    /* Read them back with ReadRune */
    for (Rune r1 = 0; r1 < NRune; r1++) {
        Int size = utf8_encode_rune(bytes_of(buf, UTF8_UTF_MAX), r1);
        Int nbytes = 0;
        Error err = BURROW_NO_ERROR;
        Rune nr = bufio_reader_read_rune(r, &nbytes, &err);
        if (nr != r1 || nbytes != size || BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ReadRune(0x%x) got 0x%x,%d not 0x%x,%d (err=%s)", r1,
                               nr, nbytes, r1, size, err);
    }
}

static void TestWriteInvalidRune(TestingT *t) {
    /* Invalid runes, including negative ones, should be written as the
     * replacement character. */
    static const Rune runes[] = {-1, UTF8_MAX_RUNE + 1};
    for (Int i = 0; i < LEN(runes); i++) {
        StringsBuilder buf = STRINGS_BUILDER(a);
        BufioWriter *w = bufio_new_writer(a, strings_builder_as_io_writer(&buf));
        (void)bufio_writer_write_rune(w, runes[i], NULL);
        (void)bufio_writer_flush(w);
        Str s = strings_builder_string(&buf);
        if (!str_eq(s, S("\xef\xbf\xbd")))
            testing_t_errorf_v(t, "WriteRune(%d) wrote %q, not replacement character",
                               runes[i], s);
    }
}

/* Go counts allocations per ReadString. The equivalent here is that ReadString
 * takes exactly one block, of exactly the line's length, from the allocator. */
static void TestReadStringAllocs(TestingT *t) {
    Str line =
        S("       foo       foo        42        42        42        42        42 "
          "       42        42        42       4.2       4.2       4.2       4.2\n");
    StringsReader sr;
    strings_reader_reset(&sr, line);
    BufioReader *buf = bufio_new_reader(heap(), strings_reader_as_io_reader(&sr));
    Int allocs = 0;
    for (int i = 0; i < 100; i++) {
        Arena run;
        arena_init(&run, NULL, 0);
        Alloc *ra = arena_allocator(&run);
        AllocStats before = mem_stats(ra);
        (void)strings_reader_seek(&sr, 0, BURROW_IO_SEEK_START, NULL);
        bufio_reader_reset(buf, strings_reader_as_io_reader(&sr));
        Error err = BURROW_NO_ERROR;
        Str s = bufio_reader_read_string(buf, ra, '\n', &err);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
        AllocStats after = mem_stats(ra);
        allocs += (Int)(after.allocs - before.allocs);
        if (!str_eq(s, line))
            testing_t_fatalf_v(t, "ReadString = %q", s);
        arena_free(&run);
    }
    if (allocs != 100)
        testing_t_errorf_v(t, "Unexpected number of allocations, got %d, want 100",
                           allocs);
    bufio_reader_free(buf);
}

static void TestWriter(TestingT *t) {
    static Byte data[8192];
    for (Int i = 0; i < LEN(data); i++)
        data[i] = (Byte)(' ' + i % ('~' - ' '));
    BytesBuffer w = BYTES_BUFFER(a);
    for (Int i = 0; i < LEN(bufsizes); i++) {
        for (Int j = 0; j < LEN(bufsizes); j++) {
            Int nwrite = bufsizes[i];
            Int bs = bufsizes[j];

            /* Write nwrite bytes using buffer size bs. Check that the right
             * amount makes it out and that the data is correct. */
            bytes_buffer_reset(&w);
            BufioWriter *buf =
                bufio_new_writer_size(heap(), bytes_buffer_as_io_writer(&w), bs);
            Error e1 = BURROW_NO_ERROR;
            Int n = bufio_writer_write(buf, bytes_of(data, nwrite), &e1);
            if (BURROW_FAILED(e1) || n != nwrite) {
                testing_t_errorf_v(t, "nwrite=%d bufsize=%d: buf.Write %d = %d, %v",
                                   nwrite, bs, nwrite, n, e1);
                bufio_writer_free(buf);
                continue;
            }
            Error e = bufio_writer_flush(buf);
            if (BURROW_FAILED(e))
                testing_t_errorf_v(t, "nwrite=%d bufsize=%d: buf.Flush = %v", nwrite,
                                   bs, e);

            Slice written = bytes_buffer_bytes(&w);
            if (written.len != nwrite)
                testing_t_errorf_v(t, "nwrite=%d bufsize=%d: %d bytes written", nwrite,
                                   bs, written.len);
            if (written.len > 0 && memcmp(written.p, data, (size_t)written.len) != 0)
                testing_t_errorf_v(t, "wrong bytes written");
            bufio_writer_free(buf);
        }
    }
}

/* Go uses math/rand with seed 0. Any fixed sequence tests the same thing, so
 * this is a small xorshift. */
static uint64_t rn_state = 88172645463325252ULL;

static Int rn_intn(Int n) {
    rn_state ^= rn_state << 13;
    rn_state ^= rn_state >> 7;
    rn_state ^= rn_state << 17;
    return (Int)(rn_state % (uint64_t)n);
}

static void TestWriterAppend(TestingT *t) {
    BytesBuffer got = BYTES_BUFFER(a);
    BytesBuffer want = BYTES_BUFFER(a);
    BufioWriter *w = bufio_new_writer_size(a, bytes_buffer_as_io_writer(&got), 64);
    for (int i = 0; i < 100; i++) {
        /* Obtain a buffer to append to. */
        Slice b = bufio_writer_available_buffer(w);
        if (bufio_writer_available(w) != b.cap)
            testing_t_fatalf_v(t, "Available() = %v, want %v",
                               bufio_writer_available(w), b.cap);

        /* While not recommended, it is valid to append to a shifted buffer.
         * This forces Write to copy the input. */
        if (rn_intn(8) == 0 && b.cap > 0) {
            b.p = (Byte *)b.p + 1;
            b.cap--;
        }

        /* Append a random integer of varying width. */
        int64_t n = (int64_t)rn_intn((Int)1 << rn_intn(30));
        char num[32];
        int k = snprintf(num, sizeof num, "%lld ", (long long)n);
        (void)bytes_buffer_write(&want, bytes_of(num, k), NULL);
        Slice out;
        if (b.cap >= k) {
            memcpy(b.p, num, (size_t)k);
            out = slice_from(b.p, k, b.cap, TYPE_BYTE);
        } else {
            /* append would move to a new array. */
            out = bytes_of(num, k);
        }
        (void)bufio_writer_write(w, out, NULL);
    }
    (void)bufio_writer_flush(w);

    if (!str_eq(text_of(bytes_buffer_bytes(&got)), text_of(bytes_buffer_bytes(&want))))
        testing_t_errorf_v(t, "output mismatch:\ngot  %s\nwant %s",
                           text_of(bytes_buffer_bytes(&got)),
                           text_of(bytes_buffer_bytes(&want)));
}

/* Check that write errors are returned properly. */
typedef struct ErrorWriterTest {
    Int n, m;
    Error err;
    Error expect;
} ErrorWriterTest;

static Int error_writer_write(void *self, Slice p, Error *err) {
    ErrorWriterTest *w = (ErrorWriterTest *)self;
    *err = w->err;
    return p.len * w->n / w->m;
}

static const IoWriterVT error_writer_vt = {&test_type, error_writer_write};

static void TestWriteErrors(TestingT *t) {
    ErrorWriterTest errorWriterTests[] = {
        {0, 1, BURROW_NO_ERROR, io_err_short_write},
        {1, 2, BURROW_NO_ERROR, io_err_short_write},
        {1, 1, BURROW_NO_ERROR, BURROW_NO_ERROR},
        {0, 1, io_err_closed_pipe, io_err_closed_pipe},
        {1, 2, io_err_closed_pipe, io_err_closed_pipe},
        {1, 1, io_err_closed_pipe, io_err_closed_pipe},
    };
    for (Int i = 0; i < LEN(errorWriterTests); i++) {
        ErrorWriterTest *w = &errorWriterTests[i];
        IoWriter iw = {&error_writer_vt, w};
        BufioWriter *buf = bufio_new_writer(a, iw);
        Error e = BURROW_NO_ERROR;
        (void)bufio_writer_write(buf, str_bytes(S("hello world")), &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Write hello to %d: %v", i, e);
            continue;
        }
        /* Two flushes, to verify the error is sticky. */
        for (int k = 0; k < 2; k++) {
            e = bufio_writer_flush(buf);
            if (!same_error(e, w->expect))
                testing_t_errorf_v(t, "Flush %d/2 %d: got %v, wanted %v", k + 1, i, e,
                                   w->expect);
        }
    }
}

static void TestNewReaderSizeIdempotent(TestingT *t) {
    enum { BufSize = 1000 };
    BufioReader *b =
        bufio_new_reader_size(heap(), strings_io(S("hello world")), BufSize);
    /* Does it recognize itself? */
    BufioReader *b1 =
        bufio_new_reader_size(heap(), bufio_reader_as_io_reader(b), BufSize);
    if (b1 != b)
        testing_t_error_v(t, "NewReaderSize did not detect underlying Reader");
    /* Does it wrap if existing buffer is too small? */
    BufioReader *b2 =
        bufio_new_reader_size(heap(), bufio_reader_as_io_reader(b), 2 * BufSize);
    if (b2 == b)
        testing_t_error_v(t, "NewReaderSize did not enlarge buffer");
    bufio_reader_free(b2);
    bufio_reader_free(b1);
    bufio_reader_free(b);
}

static void TestNewWriterSizeIdempotent(TestingT *t) {
    enum { BufSize = 1000 };
    BytesBuffer bb = BYTES_BUFFER(a);
    BufioWriter *b =
        bufio_new_writer_size(heap(), bytes_buffer_as_io_writer(&bb), BufSize);
    /* Does it recognize itself? */
    BufioWriter *b1 =
        bufio_new_writer_size(heap(), bufio_writer_as_io_writer(b), BufSize);
    if (b1 != b)
        testing_t_error_v(t, "NewWriterSize did not detect underlying Writer");
    /* Does it wrap if existing buffer is too small? */
    BufioWriter *b2 =
        bufio_new_writer_size(heap(), bufio_writer_as_io_writer(b), 2 * BufSize);
    if (b2 == b)
        testing_t_error_v(t, "NewWriterSize did not enlarge buffer");
    bufio_writer_free(b2);
    bufio_writer_free(b1);
    bufio_writer_free(b);
}

static void TestWriteString(TestingT *t) {
    enum { BufSize = 8 };
    StringsBuilder buf = STRINGS_BUILDER(a);
    BufioWriter *b =
        bufio_new_writer_size(a, strings_builder_as_io_writer(&buf), BufSize);
    (void)bufio_writer_write_string(b, S("0"), NULL);      /* easy */
    (void)bufio_writer_write_string(b, S("123456"), NULL); /* still easy */
    (void)bufio_writer_write_string(b, S("7890"), NULL);   /* easy after flush */
    (void)bufio_writer_write_string(b, S("abcdefghijklmnopqrstuvwxy"), NULL); /* hard */
    (void)bufio_writer_write_string(b, S("z"), NULL);
    Error err = bufio_writer_flush(b);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, "WriteString", err);
    Str s = S("01234567890abcdefghijklmnopqrstuvwxyz");
    if (!str_eq(strings_builder_string(&buf), s))
        testing_t_errorf_v(t, "WriteString wants %q gets %q", s,
                           strings_builder_string(&buf));
}

typedef struct TestStringWriter {
    StringsBuilder write;
    StringsBuilder write_string;
} TestStringWriter;

static Int tsw_write_string(TestStringWriter *w, Str s, Error *err) {
    return strings_builder_write_string(&w->write_string, s, err);
}

#define TSW_METHODS(M, T) M(T, WriteString, tsw_write_string, IO_SIG_WRITE_STRING)
BURROW_METHODS_DEFINE(TestStringWriter, TSW_METHODS);

static const Type tsw_type = {
    {(const Byte *)"teststringwriter", 16},
    {(const Byte *)"bufio_test", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(TestStringWriter),
    (uint16_t)_Alignof(TestStringWriter),
    0,
    (uint16_t)(sizeof burrow__methods_TestStringWriter /
               sizeof burrow__methods_TestStringWriter[0]),
    NULL,
    burrow__methods_TestStringWriter,
    NULL,
    NULL,
    0,
    0x74737772U,
    NULL,
};

static Int tsw_write(void *self, Slice p, Error *err) {
    return strings_builder_write(&((TestStringWriter *)self)->write, p, err);
}

static const IoWriterVT tsw_vt = {&tsw_type, tsw_write};

static IoWriter tsw_new(TestStringWriter *w) {
    w->write = STRINGS_BUILDER(a);
    w->write_string = STRINGS_BUILDER(a);
    IoWriter out = {&tsw_vt, w};
    return out;
}

static void tsw_check(TestingT *t, TestStringWriter *w, const char *write,
                      const char *write_string) {
    if (!str_eq(strings_builder_string(&w->write), str_from_cstr(write)))
        testing_t_errorf_v(t, "write: expected %q, got %q", write,
                           strings_builder_string(&w->write));
    if (!str_eq(strings_builder_string(&w->write_string), str_from_cstr(write_string)))
        testing_t_errorf_v(t, "writeString: expected %q, got %q", write_string,
                           strings_builder_string(&w->write_string));
}

static void TestWriteStringStringWriter(TestingT *t) {
    enum { BufSize = 8 };
    {
        TestStringWriter tw;
        BufioWriter *b = bufio_new_writer_size(a, tsw_new(&tw), BufSize);
        (void)bufio_writer_write_string(b, S("1234"), NULL);
        tsw_check(t, &tw, "", "");
        (void)bufio_writer_write_string(b, S("56789012"),
                                        NULL); /* longer than BufSize */
        tsw_check(t, &tw, "12345678", "");     /* but not enough (after filling the
                                              partially-filled buffer) */
        (void)bufio_writer_flush(b);
        tsw_check(t, &tw, "123456789012", "");
    }
    {
        TestStringWriter tw;
        BufioWriter *b = bufio_new_writer_size(a, tsw_new(&tw), BufSize);
        (void)bufio_writer_write_string(b, S("123456789"), NULL); /* long string, empty
                                                                     buffer: */
        tsw_check(t, &tw, "", "123456789");                       /* use WriteString */
    }
    {
        TestStringWriter tw;
        BufioWriter *b = bufio_new_writer_size(a, tsw_new(&tw), BufSize);
        (void)bufio_writer_write_string(b, S("abc"), NULL);
        tsw_check(t, &tw, "", "");
        (void)bufio_writer_write_string(b, S("123456789012345"), NULL); /* long string,
                                                                           non-empty buffer */
        /* use Write and then WriteString since the remaining part is still
         * longer than BufSize */
        tsw_check(t, &tw, "abc12345", "6789012345");
    }
    {
        TestStringWriter tw;
        BufioWriter *b = bufio_new_writer_size(a, tsw_new(&tw), BufSize);
        /* same as above, but use Write instead of WriteString */
        (void)bufio_writer_write(b, str_bytes(S("abc")), NULL);
        tsw_check(t, &tw, "", "");
        (void)bufio_writer_write_string(b, S("123456789012345"), NULL);
        tsw_check(t, &tw, "abc12345", "6789012345"); /* same as above */
    }
}

static void TestBufferFull(TestingT *t) {
    Str longString =
        S("And now, hello, world! It is the time for all good men to come to "
          "the aid of their party");
    BufioReader *buf =
        bufio_new_reader_size(a, strings_io(longString), MIN_READ_BUFFER_SIZE);
    Error err = BURROW_NO_ERROR;
    Slice line = bufio_reader_read_slice(buf, '!', &err);
    if (!str_eq(text_of(line), S("And now, hello, ")) ||
        !same_error(err, bufio_err_buffer_full))
        testing_t_errorf_v(t, "first ReadSlice(,) = %q, %v", text_of(line), err);
    line = bufio_reader_read_slice(buf, '!', &err);
    if (!str_eq(text_of(line), S("world!")) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "second ReadSlice(,) = %q, %v", text_of(line), err);
}

static Int data_and_eof_read(void *self, Slice p, Error *err) {
    const Str *s = (const Str *)self;
    Int n = s->len < p.len ? s->len : p.len;
    if (n > 0)
        memcpy(p.p, s->p, (size_t)n);
    *err = io_eof;
    return n;
}

static const IoReaderVT data_and_eof_vt = {&test_type, data_and_eof_read};

#define PEEK_IS(n, want)                                                               \
    do {                                                                               \
        Error e_ = BURROW_NO_ERROR;                                                    \
        Slice s_ = bufio_reader_peek(buf, n, &e_);                                     \
        if (!str_eq(text_of(s_), S(want)) || BURROW_FAILED(e_))                        \
            testing_t_fatalf_v(t, "want %q got %q, err=%v", want, text_of(s_), e_);    \
    } while (0)

#define READ_IS(len, want)                                                             \
    do {                                                                               \
        Error e_ = BURROW_NO_ERROR;                                                    \
        (void)bufio_reader_read(buf, bytes_of(p, len), &e_);                           \
        Str g_ = str_from_bytes(p, len);                                               \
        if (!str_eq(g_, S(want)) || BURROW_FAILED(e_))                                 \
            testing_t_fatalf_v(t, "want %q got %q, err=%v", want, g_, e_);             \
    } while (0)

static void TestPeek(TestingT *t) {
    Byte p[10];
    /* string is 16 (minReadBufferSize) long. */
    BufioReader *buf = bufio_new_reader_size(a, strings_io(S("abcdefghijklmnop")),
                                             MIN_READ_BUFFER_SIZE);
    PEEK_IS(1, "a");
    PEEK_IS(4, "abcd");
    Error err = BURROW_NO_ERROR;
    (void)bufio_reader_peek(buf, -1, &err);
    if (!same_error(err, bufio_err_negative_count))
        testing_t_fatalf_v(t, "want ErrNegativeCount got %v", err);
    Slice s = bufio_reader_peek(buf, 32, &err);
    if (!str_eq(text_of(s), S("abcdefghijklmnop")) ||
        !same_error(err, bufio_err_buffer_full))
        testing_t_fatalf_v(t, "want %q, ErrBufFull got %q, err=%v", "abcdefghijklmnop",
                           text_of(s), err);
    READ_IS(3, "abc");
    PEEK_IS(1, "d");
    PEEK_IS(2, "de");
    READ_IS(3, "def");
    PEEK_IS(4, "ghij");
    READ_IS(10, "ghijklmnop");
    PEEK_IS(0, "");
    (void)bufio_reader_peek(buf, 1, &err);
    if (!is_eof(err))
        testing_t_fatalf_v(t, "want EOF got %v", err);

    /* Test for issue 3022, not exposing a reader's error on a successful Peek. */
    Str abcd = S("abcd");
    IoReader de = {&data_and_eof_vt, &abcd};
    buf = bufio_new_reader_size(a, de, 32);
    s = bufio_reader_peek(buf, 2, &err);
    if (!str_eq(text_of(s), S("ab")) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Peek(2) on \"abcd\", EOF = %q, %v; want \"ab\", nil",
                           text_of(s), err);
    s = bufio_reader_peek(buf, 4, &err);
    if (!str_eq(text_of(s), S("abcd")) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Peek(4) on \"abcd\", EOF = %q, %v; want \"abcd\", nil",
                           text_of(s), err);
    Int n = bufio_reader_read(buf, bytes_of(p, 5), &err);
    if (!str_eq(str_from_bytes(p, n), S("abcd")) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read after peek = %q, %v; want abcd, EOF",
                           str_from_bytes(p, n), err);
    n = bufio_reader_read(buf, bytes_of(p, 1), &err);
    if (n != 0 || !is_eof(err))
        testing_t_fatalf_v(t, "second Read after peek = %q, %v; want \"\", EOF",
                           str_from_bytes(p, n), err);
}

static void TestPeekThenUnreadRune(TestingT *t) {
    (void)t;
    /* This sequence used to cause a crash. */
    BufioReader *r = bufio_new_reader(a, strings_io(S("x")));
    (void)bufio_reader_read_rune(r, NULL, NULL);
    (void)bufio_reader_peek(r, 1, NULL);
    (void)bufio_reader_unread_rune(r);
    (void)bufio_reader_read_rune(r, NULL, NULL); /* Used to panic here */
}

static const char testOutput[] = "0123456789abcdefghijklmnopqrstuvwxy";
static const char testInput[] =
    "012\n345\n678\n9ab\ncde\nfgh\nijk\nlmn\nopq\nrst\nuvw\nxy";
static const char testInputrn[] =
    "012\r\n345\r\n678\r\n9ab\r\ncde\r\nfgh\r\nijk\r\nlmn\r\nopq\r\n"
    "rst\r\nuvw\r\nxy\r\n\n\r\n";

/* TestReader wraps a []byte and returns reads of a specific length. */
typedef struct StrideReader {
    const Byte *data;
    Int len;
    Int stride;
} StrideReader;

static Int stride_read(void *self, Slice buf, Error *err) {
    StrideReader *r = (StrideReader *)self;
    Int n = r->stride;
    if (n > r->len)
        n = r->len;
    if (n > buf.len)
        n = buf.len;
    Int c = r->len < buf.len ? r->len : buf.len;
    if (c > 0)
        memcpy(buf.p, r->data, (size_t)c);
    r->data += n;
    r->len -= n;
    if (r->len == 0)
        *err = io_eof;
    return n;
}

static const IoReaderVT stride_vt = {&test_type, stride_read};

static void test_read_line(TestingT *t, const char *input, Int input_len) {
    Int out_len = (Int)strlen(testOutput);
    for (Int stride = 1; stride < 2; stride++) {
        Int done = 0;
        StrideReader reader = {(const Byte *)input, input_len, stride};
        IoReader ir = {&stride_vt, &reader};
        BufioReader *l = bufio_new_reader_size(a, ir, input_len + 1);
        for (;;) {
            bool is_prefix = false;
            Error err = BURROW_NO_ERROR;
            Slice line = bufio_reader_read_line(l, &is_prefix, &err);
            if (line.len > 0 && BURROW_FAILED(err))
                testing_t_errorf_v(t, "ReadLine returned both data and error: %s", err);
            if (is_prefix)
                testing_t_errorf_v(t, "ReadLine returned prefix");
            if (BURROW_FAILED(err)) {
                if (!is_eof(err))
                    testing_t_fatalf_v(t, "Got unknown error: %s", err);
                break;
            }
            Str want = str_from_bytes((const Byte *)testOutput + done, line.len);
            if (!str_eq(want, text_of(line)))
                testing_t_errorf_v(t, "Bad line at stride %d: want: %x got: %x", stride,
                                   want, text_of(line));
            done += line.len;
        }
        if (done != out_len)
            testing_t_errorf_v(t,
                               "ReadLine didn't return everything: got: %d, want: %d "
                               "(stride: %d)",
                               done, out_len, stride);
    }
}

static void TestReadLine(TestingT *t) {
    test_read_line(t, testInput, (Int)sizeof testInput - 1);
    test_read_line(t, testInputrn, (Int)sizeof testInputrn - 1);
}

static void TestLineTooLong(TestingT *t) {
    Byte data_buf[MIN_READ_BUFFER_SIZE * 5 / 2];
    for (Int i = 0; i < LEN(data_buf); i++)
        data_buf[i] = (Byte)('0' + i % 10);
    Slice data = bytes_of(data_buf, LEN(data_buf));
    BufioReader *l = bufio_new_reader_size(a, bytes_io(data), MIN_READ_BUFFER_SIZE);
    bool is_prefix = false;
    Error err = BURROW_NO_ERROR;
    Slice line = bufio_reader_read_line(l, &is_prefix, &err);
    Str want = str_from_bytes((const Byte *)data.p, MIN_READ_BUFFER_SIZE);
    if (!is_prefix || !str_eq(text_of(line), want) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "bad result for first line: got %q want %q %v",
                           text_of(line), want, err);
    data.p = (Byte *)data.p + line.len;
    data.len -= line.len;
    line = bufio_reader_read_line(l, &is_prefix, &err);
    want = str_from_bytes((const Byte *)data.p, MIN_READ_BUFFER_SIZE);
    if (!is_prefix || !str_eq(text_of(line), want) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "bad result for second line: got %q want %q %v",
                           text_of(line), want, err);
    data.p = (Byte *)data.p + line.len;
    data.len -= line.len;
    line = bufio_reader_read_line(l, &is_prefix, &err);
    want = str_from_bytes((const Byte *)data.p, MIN_READ_BUFFER_SIZE / 2);
    if (is_prefix || !str_eq(text_of(line), want) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "bad result for third line: got %q want %q %v",
                           text_of(line), want, err);
    line = bufio_reader_read_line(l, &is_prefix, &err);
    if (is_prefix || BURROW_OK(err))
        testing_t_errorf_v(t, "expected no more lines: %x %s", text_of(line), err);
}

static void TestReadAfterLines(TestingT *t) {
    Str line1 = S("this is line1");
    Str restData = S("this is line2\nthis is line 3\n");
    Str all = S("this is line1\nthis is line2\nthis is line 3\n");
    StringsBuilder outbuf = STRINGS_BUILDER(a);
    Int maxLineLength = line1.len + restData.len / 2;
    BufioReader *l = bufio_new_reader_size(a, bytes_io(str_bytes(all)), maxLineLength);
    bool is_prefix = false;
    Error err = BURROW_NO_ERROR;
    Slice line = bufio_reader_read_line(l, &is_prefix, &err);
    if (is_prefix || BURROW_FAILED(err) || !str_eq(text_of(line), line1))
        testing_t_errorf_v(t, "bad result for first line: isPrefix=%v err=%v line=%q",
                           is_prefix, err, text_of(line));
    int64_t n = io_copy(a, strings_builder_as_io_writer(&outbuf),
                        bufio_reader_as_io_reader(l), &err);
    if ((Int)n != restData.len || BURROW_FAILED(err))
        testing_t_errorf_v(t, "bad result for Read: n=%d err=%v", n, err);
    if (!str_eq(strings_builder_string(&outbuf), restData))
        testing_t_errorf_v(t, "bad result for Read: got %q; expected %q",
                           strings_builder_string(&outbuf), restData);
}

static void TestReadEmptyBuffer(TestingT *t) {
    BytesBuffer empty = BYTES_BUFFER(a);
    BufioReader *l = bufio_new_reader_size(a, bytes_buffer_as_io_reader(&empty),
                                           MIN_READ_BUFFER_SIZE);
    bool is_prefix = false;
    Error err = BURROW_NO_ERROR;
    Slice line = bufio_reader_read_line(l, &is_prefix, &err);
    if (!is_eof(err))
        testing_t_errorf_v(t, "expected EOF from ReadLine, got '%s' %t %s",
                           text_of(line), is_prefix, err);
}

static void TestLinesAfterRead(TestingT *t) {
    BufioReader *l =
        bufio_new_reader_size(a, bytes_io(str_bytes(S("foo"))), MIN_READ_BUFFER_SIZE);
    Error err = BURROW_NO_ERROR;
    (void)io_read_all(a, bufio_reader_as_io_reader(l), &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        return;
    }

    bool is_prefix = false;
    Slice line = bufio_reader_read_line(l, &is_prefix, &err);
    if (!is_eof(err))
        testing_t_errorf_v(t, "expected EOF from ReadLine, got '%s' %t %s",
                           text_of(line), is_prefix, err);
}

static void TestReadLineNonNilLineOrError(TestingT *t) {
    BufioReader *r = bufio_new_reader(a, strings_io(S("line 1\n")));
    for (int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        Slice l = bufio_reader_read_line(r, NULL, &err);
        if (l.p != NULL && BURROW_FAILED(err))
            testing_t_fatalf_v(
                t,
                "on line %d/2; ReadLine=%q, %v; want non-nil line or Error, "
                "but not both",
                i + 1, text_of(l), err);
    }
}

typedef struct ReadLineResult {
    const char *line; /* NULL for a nil line */
    bool is_prefix;
    bool eof;
} ReadLineResult;

static void test_read_line_newlines(TestingT *t, const char *input,
                                    const ReadLineResult *expect, Int n) {
    BufioReader *b = bufio_new_reader_size(a, strings_io(str_from_cstr(input)),
                                           MIN_READ_BUFFER_SIZE);
    for (Int i = 0; i < n; i++) {
        const ReadLineResult *e = &expect[i];
        bool is_prefix = false;
        Error err = BURROW_NO_ERROR;
        Slice line = bufio_reader_read_line(b, &is_prefix, &err);
        bool want_nil = e->line == NULL;
        Str want = want_nil ? S("") : str_from_cstr(e->line);
        if (!str_eq(text_of(line), want)) {
            testing_t_errorf_v(t, "%q call %d, line == %q, want %q", input, i,
                               text_of(line), want);
            return;
        }
        if (is_prefix != e->is_prefix) {
            testing_t_errorf_v(t, "%q call %d, isPrefix == %v, want %v", input, i,
                               is_prefix, e->is_prefix);
            return;
        }
        Error want_err = e->eof ? io_eof : BURROW_NO_ERROR;
        if (!same_error(err, want_err)) {
            testing_t_errorf_v(t, "%q call %d, err == %v, want %v", input, i, err,
                               want_err);
            return;
        }
    }
}

static void TestReadLineNewlines(TestingT *t) {
    static const ReadLineResult e1[] = {
        {"012345678901234", true, false},
        {NULL, false, false},
        {"012345678901234", true, false},
        {NULL, false, false},
        {NULL, false, true},
    };
    static const ReadLineResult e2[] = {
        {"0123456789012345", true, false},
        {"\r012345678901234", true, false},
        {"\r", false, false},
        {NULL, false, true},
    };
    test_read_line_newlines(t, "012345678901234\r\n012345678901234\r\n", e1, LEN(e1));
    test_read_line_newlines(t, "0123456789012345\r012345678901234\r", e2, LEN(e2));
}

static Slice create_test_input(Int n) {
    Byte *input = (Byte *)mem_alloc(a, (size_t)n, 1);
    for (Int i = 0; i < n; i++) {
        /* 101 and 251 are arbitrary prime numbers. The idea is to create an
         * input sequence which doesn't repeat too frequently. */
        input[i] = (Byte)(i % 251);
        if (i % 101 == 0)
            input[i] ^= (Byte)(i / 101);
    }
    return bytes_of(input, n);
}

/* An onlyReader only implements io.Reader, no matter what other methods the
 * underlying implementation may have. */
static Int only_read(void *self, Slice p, Error *err) {
    return BURROW_CALL(((Wrap *)self)->r, read, p, err);
}

static const IoReaderVT only_reader_vt = {&test_type, only_read};

static IoReader only_reader(IoReader r) {
    IoReader out = {&only_reader_vt, wrap(r)};
    return out;
}

/* An onlyWriter only implements io.Writer, no matter what other methods the
 * underlying implementation may have. */
typedef struct WrapW {
    IoWriter w;
} WrapW;

static Int only_write(void *self, Slice p, Error *err) {
    return BURROW_CALL(((WrapW *)self)->w, write, p, err);
}

static const IoWriterVT only_writer_vt = {&test_type, only_write};

static IoWriter only_writer(IoWriter w) {
    WrapW *x = (WrapW *)mem_alloc(a, sizeof *x, _Alignof(WrapW));
    x->w = w;
    IoWriter out = {&only_writer_vt, x};
    return out;
}

static void TestReaderWriteTo(TestingT *t) {
    Slice input = create_test_input(8192);
    BufioReader *r = bufio_new_reader(a, only_reader(bytes_io(input)));
    BytesBuffer w = BYTES_BUFFER(a);
    Error err = BURROW_NO_ERROR;
    int64_t n = bufio_reader_write_to(r, bytes_buffer_as_io_writer(&w), &err);
    if (BURROW_FAILED(err) || n != (int64_t)input.len)
        testing_t_fatalf_v(t, "r.WriteTo(w) = %d, %v, want %d, nil", n, err, input.len);

    Slice out = bytes_buffer_bytes(&w);
    for (Int i = 0; i < out.len; i++) {
        Byte val = ((const Byte *)out.p)[i];
        Byte want = ((const Byte *)input.p)[i];
        if (val != want)
            testing_t_errorf_v(t, "after write: out[%d] = %#x, want %#x", i, val, want);
    }
}

typedef struct ErrorRW {
    Int rn, wn;
    Error rerr, werr;
    Error expected;
} ErrorRW;

static Int error_rw_read(void *self, Slice p, Error *err) {
    ErrorRW *r = (ErrorRW *)self;
    *err = r->rerr;
    return p.len * r->rn;
}

static Int error_rw_write(void *self, Slice p, Error *err) {
    ErrorRW *w = (ErrorRW *)self;
    *err = w->werr;
    return p.len * w->wn;
}

static const IoReaderVT error_rw_reader_vt = {&test_type, error_rw_read};
static const IoWriterVT error_rw_writer_vt = {&test_type, error_rw_write};

static void TestReaderWriteToErrors(TestingT *t) {
    ErrorRW errorWriterToTests[] = {
        {1, 0, BURROW_NO_ERROR, io_err_closed_pipe, io_err_closed_pipe},
        {0, 1, io_err_closed_pipe, BURROW_NO_ERROR, io_err_closed_pipe},
        {0, 0, io_err_unexpected_eof, io_err_closed_pipe, io_err_unexpected_eof},
        {0, 1, io_eof, BURROW_NO_ERROR, BURROW_NO_ERROR},
    };
    for (Int i = 0; i < LEN(errorWriterToTests); i++) {
        ErrorRW *rw = &errorWriterToTests[i];
        IoReader ir = {&error_rw_reader_vt, rw};
        IoWriter iw = {&error_rw_writer_vt, rw};
        BufioReader *r = bufio_new_reader(a, ir);
        Error err = BURROW_NO_ERROR;
        (void)bufio_reader_write_to(r, iw, &err);
        if (!same_error(err, rw->expected))
            testing_t_errorf_v(t,
                               "r.WriteTo(errorWriterToTests[%d]) = _, %v, want _,%v",
                               i, err, rw->expected);
    }
}

static IoWriter ident_writer(IoWriter w) {
    return w;
}

static IoReader ident_reader(IoReader r) {
    return r;
}

static void TestWriterReadFrom(TestingT *t) {
    IoWriter (*ws[])(IoWriter) = {only_writer, ident_writer};
    IoReader (*rs[])(IoReader) = {data_err_reader, ident_reader};

    for (Int ri = 0; ri < LEN(rs); ri++) {
        for (Int wi = 0; wi < LEN(ws); wi++) {
            Slice input = create_test_input(8192);
            StringsBuilder b = STRINGS_BUILDER(a);
            BufioWriter *w =
                bufio_new_writer(a, ws[wi](strings_builder_as_io_writer(&b)));
            IoReader r = rs[ri](bytes_io(input));
            Error err = BURROW_NO_ERROR;
            int64_t n = bufio_writer_read_from(w, r, &err);
            if (BURROW_FAILED(err) || n != (int64_t)input.len) {
                testing_t_errorf_v(
                    t, "ws[%d],rs[%d]: w.ReadFrom(r) = %d, %v, want %d, nil", wi, ri, n,
                    err, input.len);
                continue;
            }
            err = bufio_writer_flush(w);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "Flush returned %v", err);
                continue;
            }
            if (!str_eq(strings_builder_string(&b), text_of(input)))
                testing_t_errorf_v(t, "ws[%d], rs[%d]: output differs", wi, ri);
        }
    }
}

static void TestWriterReadFromErrors(TestingT *t) {
    ErrorRW errorReaderFromTests[] = {
        {0, 1, io_eof, BURROW_NO_ERROR, BURROW_NO_ERROR},
        {1, 1, io_eof, BURROW_NO_ERROR, BURROW_NO_ERROR},
        {0, 1, io_err_closed_pipe, BURROW_NO_ERROR, io_err_closed_pipe},
        {0, 0, io_err_closed_pipe, io_err_short_write, io_err_closed_pipe},
        {1, 0, BURROW_NO_ERROR, io_err_short_write, io_err_short_write},
    };
    for (Int i = 0; i < LEN(errorReaderFromTests); i++) {
        ErrorRW *rw = &errorReaderFromTests[i];
        IoReader ir = {&error_rw_reader_vt, rw};
        IoWriter iw = {&error_rw_writer_vt, rw};
        BufioWriter *w = bufio_new_writer(a, iw);
        Error err = BURROW_NO_ERROR;
        (void)bufio_writer_read_from(w, ir, &err);
        if (!same_error(err, rw->expected))
            testing_t_errorf_v(
                t, "w.ReadFrom(errorReaderFromTests[%d]) = _, %v, want _,%v", i, err,
                rw->expected);
    }
}

/* A writeCountingDiscard is like io.Discard and counts the number of times
 * Write is called on it. */
static Int counting_write(void *self, Slice p, Error *err) {
    (void)err;
    (*(Int *)self)++;
    return p.len;
}

static const IoWriterVT counting_vt = {&test_type, counting_write};

static void copy_xs(BufioWriter *b, Int n) {
    StringsReader sr;
    strings_reader_reset(&sr, strings_repeat(a, S("x"), n));
    (void)io_copy(a, bufio_writer_as_io_writer(b),
                  only_reader(strings_reader_as_io_reader(&sr)), NULL);
}

/* TestWriterReadFromCounts tests that using io.Copy to copy into a
 * bufio.Writer does not prematurely flush the buffer. For example, when
 * buffering writes to a network socket, excessive network writes should be
 * avoided. */
static void TestWriterReadFromCounts(TestingT *t) {
    Int w0 = 0;
    IoWriter iw0 = {&counting_vt, &w0};
    BufioWriter *b0 = bufio_new_writer_size(a, iw0, 1234);
    (void)bufio_writer_write_string(b0, strings_repeat(a, S("x"), 1000), NULL);
    if (w0 != 0)
        testing_t_fatalf_v(t, "write 1000 'x's: got %d writes, want 0", w0);
    (void)bufio_writer_write_string(b0, strings_repeat(a, S("x"), 200), NULL);
    if (w0 != 0)
        testing_t_fatalf_v(t, "write 1200 'x's: got %d writes, want 0", w0);
    copy_xs(b0, 30);
    if (w0 != 0)
        testing_t_fatalf_v(t, "write 1230 'x's: got %d writes, want 0", w0);
    copy_xs(b0, 9);
    if (w0 != 1)
        testing_t_fatalf_v(t, "write 1239 'x's: got %d writes, want 1", w0);

    Int w1 = 0;
    IoWriter iw1 = {&counting_vt, &w1};
    BufioWriter *b1 = bufio_new_writer_size(a, iw1, 1234);
    (void)bufio_writer_write_string(b1, strings_repeat(a, S("x"), 1200), NULL);
    (void)bufio_writer_flush(b1);
    if (w1 != 1)
        testing_t_fatalf_v(t, "flush 1200 'x's: got %d writes, want 1", w1);
    (void)bufio_writer_write_string(b1, strings_repeat(a, S("x"), 89), NULL);
    if (w1 != 1)
        testing_t_fatalf_v(t, "write 1200 + 89 'x's: got %d writes, want 1", w1);
    copy_xs(b1, 700);
    if (w1 != 1)
        testing_t_fatalf_v(t, "write 1200 + 789 'x's: got %d writes, want 1", w1);
    copy_xs(b1, 600);
    if (w1 != 2)
        testing_t_fatalf_v(t, "write 1200 + 1389 'x's: got %d writes, want 2", w1);
    (void)bufio_writer_flush(b1);
    if (w1 != 3)
        testing_t_fatalf_v(t, "flush 1200 + 1389 'x's: got %d writes, want 3", w1);
}

static Int negative_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    (void)err;
    return -1;
}

static const IoReaderVT negative_vt = {&test_type, negative_read};

static void read_100(void *env) {
    Byte buf[100];
    (void)bufio_reader_read((BufioReader *)env, bytes_of(buf, 100), NULL);
}

static void TestNegativeRead(TestingT *t) {
    /* should panic with a description pointing at the reader, not at itself.
     * (should NOT panic with slice index error, for example.) */
    IoReader nr = {&negative_vt, NULL};
    BufioReader *b = bufio_new_reader(a, nr);
    Str msg;
    if (!panics(read_100, b, &msg))
        testing_t_fatal_v(t, "read did not panic");
    else if (!strings_contains(msg, S("reader returned negative count from Read")))
        testing_t_fatalf_v(t, "wrong panic: %v", msg);
}

typedef struct ErrorThenGood {
    bool did_err;
    Int nread;
} ErrorThenGood;

static Int error_then_good_read(void *self, Slice p, Error *err) {
    ErrorThenGood *r = (ErrorThenGood *)self;
    r->nread++;
    if (!r->did_err) {
        r->did_err = true;
        *err = err_fake;
        return 0;
    }
    return p.len;
}

static const IoReaderVT error_then_good_vt = {&test_type, error_then_good_read};

static void TestReaderClearError(TestingT *t) {
    ErrorThenGood r = {false, 0};
    IoReader ir = {&error_then_good_vt, &r};
    BufioReader *b = bufio_new_reader(a, ir);
    Byte buf[1];
    Error err = BURROW_NO_ERROR;
    (void)bufio_reader_read(b, slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "1st nil Read = %v; want nil", err);
    (void)bufio_reader_read(b, bytes_of(buf, 1), &err);
    if (!same_error(err, err_fake))
        testing_t_fatalf_v(t, "1st Read = %v; want errFake", err);
    (void)bufio_reader_read(b, slice_nil(TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "2nd nil Read = %v; want nil", err);
    (void)bufio_reader_read(b, bytes_of(buf, 1), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "3rd Read with buffer = %v; want nil", err);
    if (r.nread != 2)
        testing_t_errorf_v(t, "num reads = %d; want 2", r.nread);
}

/* Test for golang.org/issue/5947 */
static void TestWriterReadFromWhileFull(TestingT *t) {
    BytesBuffer buf = BYTES_BUFFER(a);
    BufioWriter *w = bufio_new_writer_size(a, bytes_buffer_as_io_writer(&buf), 10);

    /* Fill buffer exactly. */
    Error err = BURROW_NO_ERROR;
    Int n = bufio_writer_write(w, str_bytes(S("0123456789")), &err);
    if (n != 10 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write returned (%v, %v), want (10, nil)", n, err);

    /* Use ReadFrom to read in some data. */
    int64_t n2 = bufio_writer_read_from(w, strings_io(S("abcdef")), &err);
    if (n2 != 6 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadFrom returned (%v, %v), want (6, nil)", n2, err);
}

typedef struct EmptyThenNonEmpty {
    IoReader r;
    Int n;
} EmptyThenNonEmpty;

static Int etne_read(void *self, Slice p, Error *err) {
    EmptyThenNonEmpty *r = (EmptyThenNonEmpty *)self;
    if (r->n <= 0)
        return BURROW_CALL(r->r, read, p, err);
    r->n--;
    return 0;
}

static const IoReaderVT etne_vt = {&test_type, etne_read};

/* Test for golang.org/issue/7611 */
static void TestWriterReadFromUntilEOF(TestingT *t) {
    BytesBuffer buf = BYTES_BUFFER(a);
    BufioWriter *w = bufio_new_writer_size(a, bytes_buffer_as_io_writer(&buf), 5);

    /* Partially fill buffer */
    Error err = BURROW_NO_ERROR;
    Int n = bufio_writer_write(w, str_bytes(S("0123")), &err);
    if (n != 4 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write returned (%v, %v), want (4, nil)", n, err);

    /* Use ReadFrom to read in some data. */
    EmptyThenNonEmpty r = {strings_io(S("abcd")), 3};
    IoReader ir = {&etne_vt, &r};
    int64_t n2 = bufio_writer_read_from(w, ir, &err);
    if (n2 != 4 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadFrom returned (%v, %v), want (4, nil)", n2, err);
    (void)bufio_writer_flush(w);
    Str got = text_of(bytes_buffer_bytes(&buf));
    if (!str_eq(got, S("0123abcd")))
        testing_t_fatalf_v(t, "buf.Bytes() returned %q, want %q", got, "0123abcd");
}

static void TestWriterReadFromErrNoProgress(TestingT *t) {
    BytesBuffer buf = BYTES_BUFFER(a);
    BufioWriter *w = bufio_new_writer_size(a, bytes_buffer_as_io_writer(&buf), 5);

    /* Partially fill buffer */
    Error err = BURROW_NO_ERROR;
    Int n = bufio_writer_write(w, str_bytes(S("0123")), &err);
    if (n != 4 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write returned (%v, %v), want (4, nil)", n, err);

    /* Use ReadFrom to read in some data. */
    EmptyThenNonEmpty r = {strings_io(S("abcd")), 100};
    IoReader ir = {&etne_vt, &r};
    int64_t n2 = bufio_writer_read_from(w, ir, &err);
    if (n2 != 0 || !same_error(err, io_err_no_progress))
        testing_t_fatalf_v(
            t, "buf.Bytes() returned (%v, %v), want (0, io.ErrNoProgress)", n2, err);
}

typedef struct ReadFromWriter {
    BytesBuffer buf;
    Int write_bytes;
    Int read_from_bytes;
} ReadFromWriter;

static int64_t rfw_read_from(ReadFromWriter *w, IoReader r, Error *err) {
    Slice b = io_read_all(a, r, err);
    (void)bytes_buffer_write(&w->buf, b, NULL);
    w->read_from_bytes += b.len;
    return (int64_t)b.len;
}

#define RFW_METHODS(M, T) M(T, ReadFrom, rfw_read_from, IO_SIG_READ_FROM)
BURROW_METHODS_DEFINE(ReadFromWriter, RFW_METHODS);

static const Type rfw_type = {
    {(const Byte *)"readFromWriter", 14},
    {(const Byte *)"bufio_test", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(ReadFromWriter),
    (uint16_t)_Alignof(ReadFromWriter),
    0,
    (uint16_t)(sizeof burrow__methods_ReadFromWriter /
               sizeof burrow__methods_ReadFromWriter[0]),
    NULL,
    burrow__methods_ReadFromWriter,
    NULL,
    NULL,
    0,
    0x72667772U,
    NULL,
};

static Int rfw_write(void *self, Slice p, Error *err) {
    ReadFromWriter *w = (ReadFromWriter *)self;
    (void)bytes_buffer_write(&w->buf, p, err);
    w->write_bytes += p.len;
    return p.len;
}

static const IoWriterVT rfw_vt = {&rfw_type, rfw_write};

/* Test that calling (*Writer).ReadFrom with a partially-filled buffer fills
 * the buffer before switching over to ReadFrom. */
static void TestWriterReadFromWithBufferedData(TestingT *t) {
    enum { bufsize = 16, writeSize = 8 };

    Slice input = create_test_input(64);
    ReadFromWriter rfw = {BYTES_BUFFER(a), 0, 0};
    IoWriter iw = {&rfw_vt, &rfw};
    BufioWriter *w = bufio_new_writer_size(a, iw, bufsize);

    Error err = BURROW_NO_ERROR;
    Int n = bufio_writer_write(w, bytes_of(input.p, writeSize), &err);
    if (n != writeSize || BURROW_FAILED(err))
        testing_t_errorf_v(t, "w.Write(%v bytes) = %v, %v; want %v, nil",
                           (Int)writeSize, n, err, (Int)writeSize);
    Int wantn = input.len - writeSize;
    int64_t n2 = bufio_writer_read_from(
        w, bytes_io(bytes_of((Byte *)input.p + writeSize, wantn)), &err);
    if ((Int)n2 != wantn || BURROW_FAILED(err))
        testing_t_errorf_v(t, "io.Copy(w, %v bytes) = %v, %v; want %v, nil", wantn, n2,
                           err, wantn);
    err = bufio_writer_flush(w);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "w.Flush() = %v, want nil", err);

    if (rfw.write_bytes != bufsize)
        testing_t_errorf_v(t, "wrote %v bytes with Write, want %v", rfw.write_bytes,
                           (Int)bufsize);
    if (rfw.read_from_bytes != input.len - bufsize)
        testing_t_errorf_v(t, "wrote %v bytes with ReadFrom, want %v",
                           rfw.read_from_bytes, input.len - bufsize);
}

static void TestReadZero(TestingT *t) {
    static const Int sizes[] = {100, 2};
    for (Int si = 0; si < LEN(sizes); si++) {
        Int size = sizes[si];
        EmptyThenNonEmpty etne = {strings_io(S("def")), 1};
        IoReader parts[2] = {strings_io(S("abc")), {&etne_vt, &etne}};
        IoReader r = io_multi_reader(a, parts, 2);
        BufioReader *br = bufio_new_reader_size(a, r, size);
        struct {
            const char *s;
            bool eof;
        } wants[] = {{"abc", false}, {"", false}, {"def", false}, {"", true}};
        for (Int k = 0; k < LEN(wants); k++) {
            Byte p[50];
            Error err = BURROW_NO_ERROR;
            Int n = bufio_reader_read(br, bytes_of(p, 50), &err);
            Error want_err = wants[k].eof ? io_eof : BURROW_NO_ERROR;
            Str got = str_from_bytes(p, n);
            if (!same_error(err, want_err) || !str_eq(got, str_from_cstr(wants[k].s)))
                testing_t_fatalf_v(t, "bufsize=%d: read(%d) = %q, %v, want %q, %v",
                                   size, (Int)50, got, err, wants[k].s, want_err);
        }
    }
}

static void check_all(TestingT *t, BufioReader *r, const char *want) {
    Error err = BURROW_NO_ERROR;
    Slice all = io_read_all(a, bufio_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (!str_eq(text_of(all), str_from_cstr(want)))
        testing_t_errorf_v(t, "ReadAll returned %q, want %q", text_of(all), want);
}

static void TestReaderReset(TestingT *t) {
    BufioReader *r = bufio_new_reader(a, strings_io(S("foo foo")));
    Byte buf[3];
    (void)bufio_reader_read(r, bytes_of(buf, 3), NULL);
    if (!str_eq(str_from_bytes(buf, 3), S("foo")))
        testing_t_errorf_v(t, "buf = %q; want foo", str_from_bytes(buf, 3));

    bufio_reader_reset(r, strings_io(S("bar bar")));
    check_all(t, r, "bar bar");

    /* zero out the Reader. Here the zeroed one is a second value of our own,
     * since the first came from an allocator. */
    BufioReader z;
    memset(&z, 0, sizeof z);
    z.a = a;
    bufio_reader_reset(&z, strings_io(S("bar bar")));
    check_all(t, &z, "bar bar");

    /* Wrap a reader and then Reset to that reader. */
    bufio_reader_reset(&z, strings_io(S("recur")));
    BufioReader *r2 = bufio_new_reader(a, bufio_reader_as_io_reader(&z));
    check_all(t, r2, "recur");
    bufio_reader_reset(&z, strings_io(S("recur2")));
    bufio_reader_reset(r2, bufio_reader_as_io_reader(&z));
    check_all(t, r2, "recur2");
    bufio_reader_free(r2); /* r2 is &z, handed back by NewReader */
    bufio_reader_free(&z);
}

static void TestWriterReset(TestingT *t) {
    StringsBuilder buf1 = STRINGS_BUILDER(a), buf2 = STRINGS_BUILDER(a),
                   buf3 = STRINGS_BUILDER(a), buf4 = STRINGS_BUILDER(a),
                   buf5 = STRINGS_BUILDER(a);
    BufioWriter *w = bufio_new_writer(a, strings_builder_as_io_writer(&buf1));
    (void)bufio_writer_write_string(w, S("foo"), NULL);

    bufio_writer_reset(w, strings_builder_as_io_writer(&buf2)); /* and not flushed */
    (void)bufio_writer_write_string(w, S("bar"), NULL);
    (void)bufio_writer_flush(w);
    if (!str_eq(strings_builder_string(&buf1), S("")))
        testing_t_errorf_v(t, "buf1 = %q; want empty", strings_builder_string(&buf1));
    if (!str_eq(strings_builder_string(&buf2), S("bar")))
        testing_t_errorf_v(t, "buf2 = %q; want bar", strings_builder_string(&buf2));

    BufioWriter z; /* zero out the Writer */
    memset(&z, 0, sizeof z);
    bufio_writer_reset(&z, strings_builder_as_io_writer(&buf3)); /* and not flushed */
    (void)bufio_writer_write_string(&z, S("bar"), NULL);
    (void)bufio_writer_flush(&z);
    if (!str_eq(strings_builder_string(&buf1), S("")))
        testing_t_errorf_v(t, "buf1 = %q; want empty", strings_builder_string(&buf1));
    if (!str_eq(strings_builder_string(&buf3), S("bar")))
        testing_t_errorf_v(t, "buf3 = %q; want bar", strings_builder_string(&buf3));

    /* Wrap a writer and then Reset to that writer. */
    bufio_writer_reset(&z, strings_builder_as_io_writer(&buf4));
    BufioWriter *w2 = bufio_new_writer(a, bufio_writer_as_io_writer(&z));
    (void)bufio_writer_write_string(w2, S("recur"), NULL);
    (void)bufio_writer_flush(w2);
    if (!str_eq(strings_builder_string(&buf4), S("recur")))
        testing_t_errorf_v(t, "buf4 = %q, want %q", strings_builder_string(&buf4),
                           "recur");
    bufio_writer_reset(&z, strings_builder_as_io_writer(&buf5));
    bufio_writer_reset(w2, bufio_writer_as_io_writer(&z));
    (void)bufio_writer_write_string(w2, S("recur2"), NULL);
    (void)bufio_writer_flush(w2);
    if (!str_eq(strings_builder_string(&buf5), S("recur2")))
        testing_t_errorf_v(t, "buf5 = %q, want %q", strings_builder_string(&buf5),
                           "recur2");
    /* w2 is &z, handed back by NewWriter, and the zeroed writer's buffer came
     * from the heap. */
    bufio_writer_free(w2);
    bufio_writer_free(&z);
}

/* A scriptedReader is an io.Reader that executes its steps sequentially. Every
 * step here is the same one, so the script is a count. */
typedef struct Scripted {
    Int steps;
} Scripted;

static Int scripted_read(void *self, Slice p, Error *err) {
    Scripted *sr = (Scripted *)self;
    if (sr->steps == 0)
        panic_str(S("too many Read calls on scripted Reader. No steps remain."));
    sr->steps--;
    if (p.len < 5)
        panic_str(S("unexpected small read"));
    *err = err_five_then_error;
    return 5;
}

static const IoReaderVT scripted_vt = {&test_type, scripted_read};

static void TestReaderDiscard(TestingT *t) {
    typedef struct {
        const char *name;
        IoReader r;
        Int bufSize; /* 0 means 16 */
        Int peekSize;
        Int n;               /* input to Discard */
        Int want;            /* from Discard */
        const char *wantErr; /* from Discard, as text; NULL for nil */
        Int wantBuffered;
    } Case;
    Str alpha = S("abcdefghijklmnopqrstuvwxyz");
    Scripted s1 = {1}, s2 = {1}, s3 = {1}, s4 = {0}, s5 = {0};
    Case tests[] = {
        {"normal case", strings_io(alpha), 0, 16, 6, 6, NULL, 10},
        {"discard causing read", strings_io(alpha), 0, 0, 6, 6, NULL, 10},
        {"discard all without peek", strings_io(alpha), 0, 0, 26, 26, NULL, 0},
        {"discard more than end", strings_io(alpha), 0, 0, 27, 26, "EOF", 0},
        /* Any error from filling shouldn't show up until we get past the valid
         * bytes. Here we return 5 valid bytes at the same time as an error,
         * but test that we don't see the error from Discard. */
        {"fill error, discard less", {&scripted_vt, &s1}, 0, 0, 4, 4, NULL, 1},
        {"fill error, discard equal", {&scripted_vt, &s2}, 0, 0, 5, 5, NULL, 0},
        {"fill error, discard more",
         {&scripted_vt, &s3},
         0,
         0,
         6,
         5,
         "5-then-error",
         0},
        /* Discard of 0 shouldn't cause a read: */
        {"discard zero", {&scripted_vt, &s4}, 0, 0, 0, 0, NULL, 0},
        {"discard negative",
         {&scripted_vt, &s5},
         0,
         0,
         -1,
         0,
         "bufio: negative count",
         0},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        Case *tt = &tests[i];
        BufioReader *br = bufio_new_reader_size(a, tt->r, tt->bufSize);
        if (tt->peekSize > 0) {
            Error err = BURROW_NO_ERROR;
            Slice peek_buf = bufio_reader_peek(br, tt->peekSize, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: Peek(%d): %v", tt->name, tt->peekSize, err);
                continue;
            }
            if (peek_buf.len != tt->peekSize) {
                testing_t_errorf_v(t, "%s: len(Peek(%d)) = %v; want %v", tt->name,
                                   tt->peekSize, peek_buf.len, tt->peekSize);
                continue;
            }
        }
        Error err = BURROW_NO_ERROR;
        Int discarded = bufio_reader_discard(br, tt->n, &err);
        Str ge = BURROW_FAILED(err) ? error_text(err) : S("<nil>");
        Str we = tt->wantErr != NULL ? str_from_cstr(tt->wantErr) : S("<nil>");
        if (discarded != tt->want || !str_eq(ge, we)) {
            testing_t_errorf_v(t, "%s: Discard(%d) = (%v, %v); want (%v, %v)", tt->name,
                               tt->n, discarded, ge, tt->want, we);
            continue;
        }
        Int bn = bufio_reader_buffered(br);
        if (bn != tt->wantBuffered)
            testing_t_errorf_v(t, "%s: after Discard, Buffered = %d; want %d", tt->name,
                               bn, tt->wantBuffered);
    }
}

static void TestReaderSize(TestingT *t) {
    IoReader nil_reader = {NULL, NULL};
    BufioReader *r = bufio_new_reader(a, nil_reader);
    if (bufio_reader_size(r) != DEFAULT_BUF_SIZE)
        testing_t_errorf_v(t, "NewReader's Reader.Size = %d; want %d",
                           bufio_reader_size(r), (Int)DEFAULT_BUF_SIZE);
    r = bufio_new_reader_size(a, nil_reader, 1234);
    if (bufio_reader_size(r) != 1234)
        testing_t_errorf_v(t, "NewReaderSize's Reader.Size = %d; want %d",
                           bufio_reader_size(r), (Int)1234);
}

static void TestWriterSize(TestingT *t) {
    IoWriter nil_writer = {NULL, NULL};
    BufioWriter *w = bufio_new_writer(a, nil_writer);
    if (bufio_writer_size(w) != DEFAULT_BUF_SIZE)
        testing_t_errorf_v(t, "NewWriter's Writer.Size = %d; want %d",
                           bufio_writer_size(w), (Int)DEFAULT_BUF_SIZE);
    w = bufio_new_writer_size(a, nil_writer, 1234);
    if (bufio_writer_size(w) != 1234)
        testing_t_errorf_v(t, "NewWriterSize's Writer.Size = %d; want %d",
                           bufio_writer_size(w), (Int)1234);
}

/* eofReader returns the number of bytes read and io.EOF for the read that
 * consumes the last of the content. */
typedef struct EofReader {
    Byte *buf;
    Int len;
} EofReader;

static Int eof_read(void *self, Slice p, Error *err) {
    EofReader *r = (EofReader *)self;
    Int read = r->len < p.len ? r->len : p.len;
    if (read > 0)
        memcpy(p.p, r->buf, (size_t)read);
    r->buf += read;
    r->len -= read;

    /* As allowed in the documentation, this will return io.EOF in the same
     * call that consumes the last of the data. https://godoc.org/io#Reader */
    if (read == 0 || read == r->len)
        *err = io_eof;
    return read;
}

static const IoReaderVT eof_reader_vt = {&test_type, eof_read};

static void TestPartialReadEOF(TestingT *t) {
    Byte src[10] = {0};
    EofReader eofR = {src, 10};
    IoReader ir = {&eof_reader_vt, &eofR};
    BufioReader *r = bufio_new_reader(a, ir);

    /* Start by reading 5 of the 10 available bytes. */
    Byte dest[5];
    Error err = BURROW_NO_ERROR;
    Int read = bufio_reader_read(r, bytes_of(dest, 5), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected error: %v", err);
    if (read != 5)
        testing_t_fatalf_v(t, "read %d bytes; wanted %d bytes", read, (Int)5);

    /* The Reader should have buffered all the content from the io.Reader. */
    if (eofR.len != 0)
        testing_t_fatalf_v(t, "got %d bytes left in bufio.Reader source; want 0 bytes",
                           eofR.len);
    /* To prove the point, check that there are still 5 bytes available to
     * read. */
    if (bufio_reader_buffered(r) != 5)
        testing_t_fatalf_v(t, "got %d bytes buffered in bufio.Reader; want 5 bytes",
                           bufio_reader_buffered(r));

    /* This is the second read of 0 bytes. */
    read = bufio_reader_read(r, bytes_of(dest, 0), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected error: %v", err);
    if (read != 0)
        testing_t_fatalf_v(t, "read %d bytes; want 0 bytes", read);
}

typedef struct WriterWithReadFromError {
    int unused;
} WriterWithReadFromError;

static int64_t wwrfe_read_from(WriterWithReadFromError *w, IoReader r, Error *err) {
    (void)w;
    (void)r;
    *err = err_read_from_error;
    return 0;
}

#define WWRFE_METHODS(M, T) M(T, ReadFrom, wwrfe_read_from, IO_SIG_READ_FROM)
BURROW_METHODS_DEFINE(WriterWithReadFromError, WWRFE_METHODS);

static const Type wwrfe_type = {
    {(const Byte *)"writerWithReadFromError", 23},
    {(const Byte *)"bufio_test", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(WriterWithReadFromError),
    (uint16_t)_Alignof(WriterWithReadFromError),
    0,
    (uint16_t)(sizeof burrow__methods_WriterWithReadFromError /
               sizeof burrow__methods_WriterWithReadFromError[0]),
    NULL,
    burrow__methods_WriterWithReadFromError,
    NULL,
    NULL,
    0,
    0x77726665U,
    NULL,
};

static Int wwrfe_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    (void)err;
    return 10;
}

static const IoWriterVT wwrfe_vt = {&wwrfe_type, wwrfe_write};

static void TestWriterReadFromMustSetUnderlyingError(TestingT *t) {
    WriterWithReadFromError x = {0};
    IoWriter iw = {&wwrfe_vt, &x};
    BufioWriter *wr = bufio_new_writer(a, iw);
    Error err = BURROW_NO_ERROR;
    (void)bufio_writer_read_from(wr, strings_io(S("test2")), &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "expected ReadFrom returns error, got nil");
    (void)bufio_writer_write(wr, str_bytes(S("123")), &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "expected Write returns error, got nil");
}

static Int write_error_only_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = err_write_only_error;
    return 0;
}

static const IoWriterVT write_error_only_vt = {&test_type, write_error_only_write};

/* Ensure that previous Write errors are immediately returned on any ReadFrom.
 * See golang.org/issue/35194. */
static void TestWriterReadFromMustReturnUnderlyingError(TestingT *t) {
    IoWriter iw = {&write_error_only_vt, NULL};
    BufioWriter *wr = bufio_new_writer(a, iw);
    Str s = S("test1");
    Int wantBuffered = s.len;
    Error err = BURROW_NO_ERROR;
    (void)bufio_writer_write_string(wr, s, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected error: %v", err);
    if (BURROW_OK(bufio_writer_flush(wr)))
        testing_t_error_v(t, "expected flush error, got nil");
    (void)bufio_writer_read_from(wr, strings_io(S("test2")), &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "expected error, got nil");
    Int buffered = bufio_writer_buffered(wr);
    if (buffered != wantBuffered)
        testing_t_fatalf_v(t, "Buffered = %v; want %v", buffered, wantBuffered);
}

/* Not in Go. A ReadWriter passes everything through to its two halves, and
 * converts to the three io interfaces. */
static void TestReadWriter(TestingT *t) {
    BytesBuffer out = BYTES_BUFFER(a);
    BufioReader *r = bufio_new_reader(a, strings_io(S("one\ntwo\n")));
    BufioWriter *w = bufio_new_writer(a, bytes_buffer_as_io_writer(&out));
    BufioReadWriter rw = bufio_new_read_writer(r, w);
    Error err = BURROW_NO_ERROR;
    Str line = bufio_read_writer_read_string(rw, a, '\n', &err);
    CHECK(str_eq(line, S("one\n")) && BURROW_OK(err));
    (void)bufio_read_writer_write_string(rw, S("got "), NULL);
    (void)bufio_read_writer_write(rw, str_bytes(line), NULL);
    CHECK(bufio_read_writer_available(rw) == DEFAULT_BUF_SIZE - 8);
    CHECK(bytes_buffer_len(&out) == 0);
    int64_t n = io_copy(a, bufio_read_writer_as_io_writer(&rw),
                        bufio_read_writer_as_io_reader(&rw), &err);
    CHECK(n == 4 && BURROW_OK(err));
    CHECK(BURROW_OK(bufio_read_writer_flush(rw)));
    CHECK(str_eq(text_of(bytes_buffer_bytes(&out)), S("got one\ntwo\n")));
    IoReadWriter irw = bufio_read_writer_as_io_read_writer(&rw);
    CHECK(irw.data == &rw);
}

/* Not in Go. Freeing a reader handed back twice by NewReaderSize only gives it
 * back on the last call, which the heap checks under the sanitizers. */
static void TestReaderRefs(TestingT *t) {
    BufioReader *b = bufio_new_reader_size(heap(), strings_io(S("x")), 64);
    BufioReader *b1 = bufio_new_reader_size(heap(), bufio_reader_as_io_reader(b), 32);
    CHECK(b1 == b);
    bufio_reader_free(b1);
    CHECK(bufio_reader_read_byte(b, NULL) == 'x');
    bufio_reader_free(b);
}

#define TESTS(X)                                                                       \
    X(TestReaderSimple)                                                                \
    X(TestReader)                                                                      \
    X(TestZeroReader)                                                                  \
    X(TestReadRune)                                                                    \
    X(TestUnreadRune)                                                                  \
    X(TestNoUnreadRuneAfterPeek)                                                       \
    X(TestNoUnreadByteAfterPeek)                                                       \
    X(TestNoUnreadRuneAfterDiscard)                                                    \
    X(TestNoUnreadByteAfterDiscard)                                                    \
    X(TestNoUnreadRuneAfterWriteTo)                                                    \
    X(TestNoUnreadByteAfterWriteTo)                                                    \
    X(TestUnreadByte)                                                                  \
    X(TestUnreadByteMultiple)                                                          \
    X(TestUnreadByteOthers)                                                            \
    X(TestUnreadRuneError)                                                             \
    X(TestUnreadRuneAtEOF)                                                             \
    X(TestReadWriteRune)                                                               \
    X(TestWriteInvalidRune)                                                            \
    X(TestReadStringAllocs)                                                            \
    X(TestWriter)                                                                      \
    X(TestWriterAppend)                                                                \
    X(TestWriteErrors)                                                                 \
    X(TestNewReaderSizeIdempotent)                                                     \
    X(TestNewWriterSizeIdempotent)                                                     \
    X(TestWriteString)                                                                 \
    X(TestWriteStringStringWriter)                                                     \
    X(TestBufferFull)                                                                  \
    X(TestPeek)                                                                        \
    X(TestPeekThenUnreadRune)                                                          \
    X(TestReadLine)                                                                    \
    X(TestLineTooLong)                                                                 \
    X(TestReadAfterLines)                                                              \
    X(TestReadEmptyBuffer)                                                             \
    X(TestLinesAfterRead)                                                              \
    X(TestReadLineNonNilLineOrError)                                                   \
    X(TestReadLineNewlines)                                                            \
    X(TestReaderWriteTo)                                                               \
    X(TestReaderWriteToErrors)                                                         \
    X(TestWriterReadFrom)                                                              \
    X(TestWriterReadFromErrors)                                                        \
    X(TestWriterReadFromCounts)                                                        \
    X(TestNegativeRead)                                                                \
    X(TestReaderClearError)                                                            \
    X(TestWriterReadFromWhileFull)                                                     \
    X(TestWriterReadFromUntilEOF)                                                      \
    X(TestWriterReadFromErrNoProgress)                                                 \
    X(TestWriterReadFromWithBufferedData)                                              \
    X(TestReadZero)                                                                    \
    X(TestReaderReset)                                                                 \
    X(TestWriterReset)                                                                 \
    X(TestReaderDiscard)                                                               \
    X(TestReaderSize)                                                                  \
    X(TestWriterSize)                                                                  \
    X(TestPartialReadEOF)                                                              \
    X(TestWriterReadFromMustSetUnderlyingError)                                        \
    X(TestWriterReadFromMustReturnUnderlyingError)                                     \
    X(TestReadWriter)                                                                  \
    X(TestReaderRefs)

static int TestMain(TestingM *m) {
    setup();
    int code = testing_m_run(m);
    teardown();
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
