/* Derived from Go's src/bytes/buffer_test.go.
 * Go source: go1.27.1.
 *
 * Every test runs its buffers on a Track, and checks at the end that freeing
 * them gave back everything they took. Go counts allocations with
 * testing.AllocsPerRun, and the Track counts them here in the same way: one
 * warm up call, then the average over the runs, rounded down.
 *
 * TestNewBufferShallow is left out. It checks that Go's escape analysis keeps
 * `buf = *NewBuffer(b)` off the heap, and bytes_new_buffer always allocates the
 * buffer it returns, where a caller who wants none writes a BytesBuffer value.
 * TestGrowOverflow checks for the error that Go panics with, since growing
 * here returns false instead.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

#define N 10000            /* make this bigger for a larger (and slower) test */
static Byte test_bytes[N]; /* test data for write tests */

static void init_test_bytes(void) {
    for (Int i = 0; i < N; i++)
        test_bytes[i] = (Byte)('a' + i % 26);
}

static Slice tb(Int lo, Int hi) {
    return slice_from(test_bytes + lo, hi - lo, hi - lo, TYPE_BYTE);
}

static Str ts(Int lo, Int hi) {
    return str_from_bytes(test_bytes + lo, hi - lo);
}

static Str sv(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static Track track;

static Alloc *begin(void) {
    init_test_bytes();
    track_init(&track, heap_allocator());
    return track_allocator(&track);
}

static void end(TestingT *t) {
    if (track_check(&track) != 0)
        testing_t_errorf_v(t, "the buffers leaked or misused memory: %d faults",
                           track_faults(&track));
    track_free(&track);
}

/* Go builds up the expected contents with s += fus. This keeps them in a
 * growable array from the heap. */
typedef struct Want {
    Byte *p;
    Int len, cap;
} Want;

static void want_add(Want *w, Str s) {
    if (w->len + s.len > w->cap) {
        Int nc = 2 * w->cap + s.len;
        w->p =
            (Byte *)mem_realloc(heap_allocator(), w->p, (size_t)w->cap, (size_t)nc, 1);
        w->cap = nc;
    }
    if (s.len > 0)
        memcpy(w->p + w->len, s.p, (size_t)s.len);
    w->len += s.len;
}

static Str want_str(const Want *w) {
    return str_from_bytes(w->p, w->len);
}

static void want_drop(Want *w, Int n) {
    memmove(w->p, w->p + n, (size_t)(w->len - n));
    w->len -= n;
}

static void want_free(Want *w) {
    if (w->p != NULL)
        mem_free(heap_allocator(), w->p, (size_t)w->cap, 1);
    *w = (Want){0};
}

/* Verify that contents of buf match the string s. */
static void check(TestingT *t, const char *testname, BytesBuffer *buf, Str s) {
    Slice bytes = bytes_buffer_bytes(buf);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str str = bytes_buffer_string(buf, arena_allocator(&ar));
    if (bytes_buffer_len(buf) != bytes.len)
        testing_t_errorf_v(t, "%s: buf.Len() == %d, len(buf.Bytes()) == %d", testname,
                           bytes_buffer_len(buf), bytes.len);
    if (bytes_buffer_len(buf) != str.len)
        testing_t_errorf_v(t, "%s: buf.Len() == %d, len(buf.String()) == %d", testname,
                           bytes_buffer_len(buf), str.len);
    if (bytes_buffer_len(buf) != s.len)
        testing_t_errorf_v(t, "%s: buf.Len() == %d, len(s) == %d", testname,
                           bytes_buffer_len(buf), s.len);
    if (!str_eq(sv(bytes), s))
        testing_t_errorf_v(t, "%s: string(buf.Bytes()) == %q, s == %q", testname,
                           sv(bytes), s);
    arena_free(&ar);
}

/* Fill buf through n writes of string fus. The initial contents of buf
 * corresponds to s, which ends up as the final contents. */
static void fill_string(TestingT *t, const char *testname, BytesBuffer *buf, Want *s,
                        Int n, Str fus) {
    check(t, testname, buf, want_str(s));
    for (; n > 0; n--) {
        Error err;
        Int m = bytes_buffer_write_string(buf, fus, &err);
        if (m != fus.len)
            testing_t_errorf_v(t, "%s (fill 2): m == %d, expected %d", testname, m,
                               fus.len);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t,
                               "%s (fill 3): err should always be nil, found err == %v",
                               testname, err);
        want_add(s, fus);
        check(t, testname, buf, want_str(s));
    }
}

/* Fill buf through n writes of byte slice fub. */
static void fill_bytes(TestingT *t, const char *testname, BytesBuffer *buf, Want *s,
                       Int n, Slice fub) {
    check(t, testname, buf, want_str(s));
    for (; n > 0; n--) {
        Error err;
        Int m = bytes_buffer_write(buf, fub, &err);
        if (m != fub.len)
            testing_t_errorf_v(t, "%s (fill 2): m == %d, expected %d", testname, m,
                               fub.len);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t,
                               "%s (fill 3): err should always be nil, found err == %v",
                               testname, err);
        want_add(s, sv(fub));
        check(t, testname, buf, want_str(s));
    }
}

static void TestNewBuffer(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer *buf = bytes_new_buffer(a, tb(0, N));
    check(t, "NewBuffer", buf, ts(0, N));
    bytes_buffer_free(buf);
    end(t);
}

static void TestNewBufferString(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer *buf = bytes_new_buffer_string(a, ts(0, N));
    check(t, "NewBufferString", buf, ts(0, N));
    bytes_buffer_free(buf);
    end(t);
}

/* Empty buf through repeated reads into fub. The initial contents of buf
 * corresponds to the string s. */
static void empty(TestingT *t, const char *testname, BytesBuffer *buf, Str s,
                  Slice fub) {
    check(t, testname, buf, s);
    for (;;) {
        Error err;
        Int n = bytes_buffer_read(buf, fub, &err);
        if (n == 0)
            break;
        if (BURROW_FAILED(err))
            testing_t_errorf_v(
                t, "%s (empty 2): err should always be nil, found err == %v", testname,
                err);
        s = str_from_bytes(s.p + n, s.len - n);
        check(t, testname, buf, s);
    }
    check(t, testname, buf, S(""));
}

static Byte scratch[N];

static Slice make_bytes(Int n) {
    memset(scratch, 0, (size_t)n);
    return slice_from(scratch, n, n, TYPE_BYTE);
}

static void TestBasicOperations(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    for (int i = 0; i < 5; i++) {
        check(t, "TestBasicOperations (1)", &buf, S(""));

        bytes_buffer_reset(&buf);
        check(t, "TestBasicOperations (2)", &buf, S(""));

        bytes_buffer_truncate(&buf, 0);
        check(t, "TestBasicOperations (3)", &buf, S(""));

        Error err;
        Int n = bytes_buffer_write(&buf, tb(0, 1), &err);
        if (BURROW_FAILED(err) || n != 1)
            testing_t_errorf_v(t, "Write: got (%d, %v), want (%d, %v)", n, err, 1,
                               BURROW_NO_ERROR);
        check(t, "TestBasicOperations (4)", &buf, S("a"));

        (void)bytes_buffer_write_byte(&buf, test_bytes[1]);
        check(t, "TestBasicOperations (5)", &buf, S("ab"));

        n = bytes_buffer_write(&buf, tb(2, 26), &err);
        if (BURROW_FAILED(err) || n != 24)
            testing_t_errorf_v(t, "Write: got (%d, %v), want (%d, %v)", n, err, 24,
                               BURROW_NO_ERROR);
        check(t, "TestBasicOperations (6)", &buf, ts(0, 26));

        bytes_buffer_truncate(&buf, 26);
        check(t, "TestBasicOperations (7)", &buf, ts(0, 26));

        bytes_buffer_truncate(&buf, 20);
        check(t, "TestBasicOperations (8)", &buf, ts(0, 20));

        empty(t, "TestBasicOperations (9)", &buf, ts(0, 20), make_bytes(5));
        empty(t, "TestBasicOperations (10)", &buf, S(""), make_bytes(100));

        (void)bytes_buffer_write_byte(&buf, test_bytes[1]);
        Byte c = bytes_buffer_read_byte(&buf, &err);
        if (BURROW_FAILED(err) || c != test_bytes[1])
            testing_t_errorf_v(t, "ReadByte: got (%q, %v), want (%q, %v)", c, err,
                               test_bytes[1], BURROW_NO_ERROR);
        c = bytes_buffer_read_byte(&buf, &err);
        if (!errors_is(err, io_eof))
            testing_t_errorf_v(t, "ReadByte: got (%q, %v), want (%q, %v)", c, err,
                               (Byte)0, io_eof);
    }
    bytes_buffer_free(&buf);
    end(t);
}

static void TestLargeStringWrites(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    Int limit = 30;
    if (testing_short())
        limit = 9;
    for (Int i = 3; i < limit; i += 3) {
        Want s = {0};
        fill_string(t, "TestLargeWrites (1)", &buf, &s, 5, ts(0, N));
        empty(t, "TestLargeStringWrites (2)", &buf, want_str(&s), make_bytes(N / i));
        want_free(&s);
    }
    check(t, "TestLargeStringWrites (3)", &buf, S(""));
    bytes_buffer_free(&buf);
    end(t);
}

static void TestLargeByteWrites(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    Int limit = 30;
    if (testing_short())
        limit = 9;
    for (Int i = 3; i < limit; i += 3) {
        Want s = {0};
        fill_bytes(t, "TestLargeWrites (1)", &buf, &s, 5, tb(0, N));
        empty(t, "TestLargeByteWrites (2)", &buf, want_str(&s), make_bytes(N / i));
        want_free(&s);
    }
    check(t, "TestLargeByteWrites (3)", &buf, S(""));
    bytes_buffer_free(&buf);
    end(t);
}

static void TestLargeStringReads(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    for (Int i = 3; i < 30; i += 3) {
        Want s = {0};
        fill_string(t, "TestLargeReads (1)", &buf, &s, 5, ts(0, N / i));
        empty(t, "TestLargeReads (2)", &buf, want_str(&s), make_bytes(N));
        want_free(&s);
    }
    check(t, "TestLargeStringReads (3)", &buf, S(""));
    bytes_buffer_free(&buf);
    end(t);
}

static void TestLargeByteReads(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    for (Int i = 3; i < 30; i += 3) {
        Want s = {0};
        fill_bytes(t, "TestLargeReads (1)", &buf, &s, 5, tb(0, N / i));
        empty(t, "TestLargeReads (2)", &buf, want_str(&s), make_bytes(N));
        want_free(&s);
    }
    check(t, "TestLargeByteReads (3)", &buf, S(""));
    bytes_buffer_free(&buf);
    end(t);
}

/* math/rand's Intn stands in as a small xorshift with a fixed seed. */
static uint64_t rand_state = 0x9e3779b97f4a7c15ULL;

static Int rand_intn(Int n) {
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 7;
    rand_state ^= rand_state << 17;
    return (Int)(rand_state % (uint64_t)n);
}

static void TestMixedReadsAndWrites(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    Want s = {0};
    for (Int i = 0; i < 50; i++) {
        Int wlen = rand_intn(N);
        if (i % 2 == 0)
            fill_string(t, "TestMixedReadsAndWrites (1)", &buf, &s, 1, ts(0, wlen));
        else
            fill_bytes(t, "TestMixedReadsAndWrites (1)", &buf, &s, 1, tb(0, wlen));
        Int rlen = rand_intn(N);
        Int n = bytes_buffer_read(&buf, make_bytes(rlen), NULL);
        want_drop(&s, n);
    }
    empty(t, "TestMixedReadsAndWrites (2)", &buf, want_str(&s),
          make_bytes(bytes_buffer_len(&buf)));
    want_free(&s);
    bytes_buffer_free(&buf);
    end(t);
}

static void TestCapWithPreallocatedSlice(TestingT *t) {
    Alloc *a = begin();
    Byte ten[10] = {0};
    BytesBuffer *buf = bytes_new_buffer(a, slice_from(ten, 10, 10, TYPE_BYTE));
    Int n = bytes_buffer_cap(buf);
    if (n != 10)
        testing_t_errorf_v(t, "expected 10, got %d", n);
    bytes_buffer_free(buf);
    end(t);
}

static void TestCapWithSliceAndWrittenData(TestingT *t) {
    Alloc *a = begin();
    Byte ten[10];
    BytesBuffer *buf = bytes_new_buffer(a, slice_from(ten, 0, 10, TYPE_BYTE));
    bytes_buffer_write(buf, BURROW_B("test"), NULL);
    Int n = bytes_buffer_cap(buf);
    if (n != 10)
        testing_t_errorf_v(t, "expected 10, got %d", n);
    bytes_buffer_free(buf);
    end(t);
}

static void TestNil(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str s = bytes_buffer_string(NULL, arena_allocator(&ar));
    if (!str_eq(s, S("<nil>")))
        testing_t_errorf_v(t, "expected <nil>; got %q", s);
    arena_free(&ar);
}

static void TestReadFrom(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    for (Int i = 3; i < 30; i += 3) {
        Want s = {0};
        fill_bytes(t, "TestReadFrom (1)", &buf, &s, 5, tb(0, N / i));
        BytesBuffer b = BYTES_BUFFER(a);
        bytes_buffer_read_from(&b, bytes_buffer_as_io_reader(&buf), NULL);
        empty(t, "TestReadFrom (2)", &b, want_str(&s), make_bytes(N));
        bytes_buffer_free(&b);
        want_free(&s);
    }
    bytes_buffer_free(&buf);
    end(t);
}

static Int panic_read(void *self, Slice p, Error *err) {
    if (*(bool *)self)
        panic_str(S("oops"));
    BURROW_OUT(err, io_eof);
    return 0;
}

static const IoReaderVT panic_reader_vt = {NULL, panic_read};

typedef struct ReadFromCall {
    BytesBuffer *b;
    IoReader r;
} ReadFromCall;

static void call_read_from(void *env) {
    ReadFromCall *c = (ReadFromCall *)env;
    bytes_buffer_read_from(c->b, c->r, NULL);
}

/* What a call panicked with. The value belongs to the panic and is gone once
 * the catch block ends, so an Error is copied out and anything else leaves
 * just its type and text. */
typedef struct Recovered {
    const Type *t; /* NULL if it returned */
    Error err;
    char text[256];
} Recovered;

static Recovered recovered(Func f) {
    static Recovered got;
    memset(&got, 0, sizeof got);
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        got.t = r.t;
        if (r.t == TYPE_ERROR)
            got.err = *(const Error *)r.data;
        Str s = panic_text(r);
        size_t n = s.len < (Int)sizeof got.text ? (size_t)s.len : sizeof got.text - 1;
        memcpy(got.text, s.p, n);
    }
    BURROW_TRY_END;
    return got;
}

/* Make sure that an empty Buffer remains empty when it is "grown" before a
 * Read that panics. */
static void TestReadFromPanicReader(TestingT *t) {
    Alloc *a = begin();
    /* First verify non-panic behaviour */
    BytesBuffer buf = BYTES_BUFFER(a);
    bool no = false;
    Error err;
    int64_t i = bytes_buffer_read_from(&buf, (IoReader){&panic_reader_vt, &no}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (i != 0)
        testing_t_fatalf_v(
            t, "unexpected return from bytes.ReadFrom (1): got: %d, want %d", i, 0);
    check(t, "TestReadFromPanicReader (1)", &buf, S(""));

    /* Confirm that when Reader panics, the empty buffer remains empty */
    BytesBuffer buf2 = BYTES_BUFFER(a);
    bool yes = true;
    ReadFromCall c = {&buf2, {&panic_reader_vt, &yes}};
    recovered(BURROW_FN(Func, call_read_from, &c));
    check(t, "TestReadFromPanicReader (2)", &buf2, S(""));
    bytes_buffer_free(&buf);
    bytes_buffer_free(&buf2);
    end(t);
}

static Int negative_read(void *self, Slice p, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return -1;
}

static const IoReaderVT negative_reader_vt = {NULL, negative_read};

static void TestReadFromNegativeReader(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer b = BYTES_BUFFER(a);
    ReadFromCall c = {&b, {&negative_reader_vt, NULL}};
    Recovered r = recovered(BURROW_FN(Func, call_read_from, &c));
    if (r.t == NULL) {
        testing_t_fatal_v(t, "bytes.Buffer.ReadFrom didn't panic");
    } else if (r.t == TYPE_ERROR) {
        /* this is the error string of errNegativeRead */
        Str want_error = S("bytes.Buffer: reader returned negative count from Read");
        Str got = error_text(r.err);
        if (!str_eq(got, want_error))
            testing_t_fatalf_v(t, "recovered panic: got %v, want %v", got, want_error);
    } else {
        testing_t_fatalf_v(t, "unexpected panic value: %s", str_from_cstr(r.text));
    }
    bytes_buffer_free(&b);
    end(t);
}

static void TestWriteTo(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer buf = BYTES_BUFFER(a);
    for (Int i = 3; i < 30; i += 3) {
        Want s = {0};
        fill_bytes(t, "TestWriteTo (1)", &buf, &s, 5, tb(0, N / i));
        BytesBuffer b = BYTES_BUFFER(a);
        bytes_buffer_write_to(&buf, bytes_buffer_as_io_writer(&b), NULL);
        empty(t, "TestWriteTo (2)", &b, want_str(&s), make_bytes(N));
        bytes_buffer_free(&b);
        want_free(&s);
    }
    bytes_buffer_free(&buf);
    end(t);
}

static void TestWriteAppend(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer got = BYTES_BUFFER(a);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice want = slice_nil(TYPE_BYTE);
    for (int64_t i = 0; i < 1000; i++) {
        Slice b = bytes_buffer_available_buffer(&got);
        /* An append that outgrows the free space goes to the arena. */
        b = strconv_append_int(arena_allocator(&ar), b, i, 10);
        want = strconv_append_int(arena_allocator(&ar), want, i, 10);
        bytes_buffer_write(&got, b, NULL);
    }
    if (!bytes_equal(bytes_buffer_bytes(&got), want))
        testing_t_fatalf_v(t, "Bytes() = %q, want %q", sv(bytes_buffer_bytes(&got)),
                           sv(want));

    /* With a sufficiently sized buffer, there should be no allocations. */
    uint64_t before = track.allocs;
    bytes_buffer_reset(&got);
    for (int64_t i = 0; i < 1000; i++) {
        Slice b = bytes_buffer_available_buffer(&got);
        b = strconv_append_int(a, b, i, 10);
        bytes_buffer_write(&got, b, NULL);
    }
    if (track.allocs != before)
        testing_t_errorf_v(t, "allocations occurred while appending");
    arena_free(&ar);
    bytes_buffer_free(&got);
    end(t);
}

static void TestRuneIO(TestingT *t) {
    Alloc *a = begin();
    enum { NRune = 1000 };
    /* Built a test slice while we write the data */
    static Byte b[UTF8_UTF_MAX * NRune];
    BytesBuffer buf = BYTES_BUFFER(a);
    Int n = 0;
    for (Rune r = 0; r < NRune; r++) {
        Int size = utf8_encode_rune(
            slice_from(b + n, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
        Error err;
        Int nbytes = bytes_buffer_write_rune(&buf, r, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "WriteRune(%U) error: %v", r, err);
        if (nbytes != size)
            testing_t_fatalf_v(t, "WriteRune(%U) expected %d, got %d", r, size, nbytes);
        n += size;
    }
    Slice bs = slice_from(b, n, n, TYPE_BYTE);

    /* Check the resulting bytes */
    if (!bytes_equal(bytes_buffer_bytes(&buf), bs))
        testing_t_fatalf_v(t, "incorrect result from WriteRune: %q not %q",
                           sv(bytes_buffer_bytes(&buf)), sv(bs));

    Byte p[UTF8_UTF_MAX];
    /* Read it back with ReadRune */
    for (Rune r = 0; r < NRune; r++) {
        Int size =
            utf8_encode_rune(slice_from(p, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
        Int nbytes;
        Error err;
        Rune nr = bytes_buffer_read_rune(&buf, &nbytes, &err);
        if (nr != r || nbytes != size || BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ReadRune(%U) got %U,%d not %U,%d (err=%v)", r, nr,
                               nbytes, r, size, err);
    }

    /* Check that UnreadRune works */
    bytes_buffer_reset(&buf);

    /* check at EOF */
    if (BURROW_OK(bytes_buffer_unread_rune(&buf)))
        testing_t_fatal_v(t, "UnreadRune at EOF: got no error");
    Error err;
    bytes_buffer_read_rune(&buf, NULL, &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "ReadRune at EOF: got no error");
    if (BURROW_OK(bytes_buffer_unread_rune(&buf)))
        testing_t_fatal_v(t, "UnreadRune after ReadRune at EOF: got no error");

    /* check not at EOF */
    bytes_buffer_write(&buf, bs, NULL);
    for (Rune r = 0; r < NRune; r++) {
        Int size, nbytes;
        Rune r1 = bytes_buffer_read_rune(&buf, &size, NULL);
        err = bytes_buffer_unread_rune(&buf);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "UnreadRune(%U) got error %q", r, error_text(err));
        Rune r2 = bytes_buffer_read_rune(&buf, &nbytes, &err);
        if (r1 != r2 || r1 != r || nbytes != size || BURROW_FAILED(err))
            testing_t_fatalf_v(
                t, "ReadRune(%U) after UnreadRune got %U,%d not %U,%d (err=%v)", r, r2,
                nbytes, r, size, err);
    }
    bytes_buffer_free(&buf);
    end(t);
}

static void TestWriteInvalidRune(TestingT *t) {
    Alloc *a = begin();
    /* Invalid runes, including negative ones, should be written as
     * utf8.RuneError. */
    static const Rune runes[] = {-1, UTF8_MAX_RUNE + 1};
    for (Int i = 0; i < LEN(runes); i++) {
        BytesBuffer buf = BYTES_BUFFER(a);
        bytes_buffer_write_rune(&buf, runes[i], NULL);
        check(t, "TestWriteInvalidRune", &buf, S("\xEF\xBF\xBD"));
        bytes_buffer_free(&buf);
    }
    end(t);
}

static void TestNext(TestingT *t) {
    Alloc *a = begin();
    Byte b[] = {0, 1, 2, 3, 4};
    Byte tmp[5];
    for (Int i = 0; i <= 5; i++) {
        for (Int j = i; j <= 5; j++) {
            for (Int k = 0; k <= 6; k++) {
                /* 0 <= i <= j <= 5; 0 <= k <= 6
                 * Check that if we start with a buffer of length j at offset i
                 * and ask for Next(k), we get the right bytes. */
                BytesBuffer *buf = bytes_new_buffer(a, slice_from(b, j, 5, TYPE_BYTE));
                Int n = bytes_buffer_read(buf, slice_from(tmp, i, 5, TYPE_BYTE), NULL);
                if (n != i)
                    testing_t_fatalf_v(t, "Read %d returned %d", i, n);
                Slice bb = bytes_buffer_next(buf, k);
                Int want = k;
                if (want > j - i)
                    want = j - i;
                if (bb.len != want)
                    testing_t_fatalf_v(t, "in %d,%d: len(Next(%d)) == %d", i, j, k,
                                       bb.len);
                for (Int l = 0; l < bb.len; l++) {
                    Byte v = ((const Byte *)bb.p)[l];
                    if (v != (Byte)(l + i))
                        testing_t_fatalf_v(t, "in %d,%d: Next(%d)[%d] = %d, want %d", i,
                                           j, k, l, v, l + i);
                }
                bytes_buffer_free(buf);
            }
        }
    }
    end(t);
}

static const struct {
    Str buffer;
    Byte delim;
    const char *expected[4];
    bool eof;
} readBytesTests[] = {
    {BURROW_S_INIT(""), 0, {""}, true},
    {BURROW_S_INIT("a\x00"), 0, {"a"}, false}, /* "a\x00", see want_line */
    {BURROW_S_INIT("abbbaaaba"), 'b', {"ab", "b", "b", "aaab"}, false},
    {BURROW_S_INIT("hello\x01world"), 1, {"hello\x01"}, false},
    {BURROW_S_INIT("foo\nbar"), 0, {"foo\nbar"}, true},
    {BURROW_S_INIT("alpha\nbeta\ngamma\n"),
     '\n',
     {"alpha\n", "beta\n", "gamma\n"},
     false},
    {BURROW_S_INIT("alpha\nbeta\ngamma"), '\n', {"alpha\n", "beta\n", "gamma"}, true},
};

/* The expected lines as Str. A C string cannot hold the NUL that ends Go's
 * "a\x00", so a line ending in the delimiter 0 gets it back. */
static Str want_line(Int i, Int j) {
    const char *e = readBytesTests[i].expected[j];
    Str s = str_from_cstr(e);
    if (readBytesTests[i].delim == 0 && !readBytesTests[i].eof)
        s.len++;
    return s;
}

static Int expected_count(Int i) {
    Int n = 0;
    while (n < 4 && readBytesTests[i].expected[n] != NULL)
        n++;
    return n;
}

static void TestReadBytes(TestingT *t) {
    Alloc *a = begin();
    for (Int i = 0; i < LEN(readBytesTests); i++) {
        BytesBuffer *buf = bytes_new_buffer_string(a, readBytesTests[i].buffer);
        Error err = BURROW_NO_ERROR;
        for (Int j = 0; j < expected_count(i); j++) {
            Slice bytes =
                bytes_buffer_read_bytes(buf, a, readBytesTests[i].delim, &err);
            if (!str_eq(sv(bytes), want_line(i, j)))
                testing_t_errorf_v(t, "expected %q, got %q", want_line(i, j),
                                   sv(bytes));
            if (bytes.p != NULL)
                mem_free(a, bytes.p, (size_t)bytes.cap, 1);
            if (BURROW_FAILED(err))
                break;
        }
        if (!errors_is(err, readBytesTests[i].eof ? io_eof : BURROW_NO_ERROR))
            testing_t_errorf_v(t, "expected error %v, got %v",
                               readBytesTests[i].eof ? io_eof : BURROW_NO_ERROR, err);
        bytes_buffer_free(buf);
    }
    end(t);
}

static void TestReadString(TestingT *t) {
    Alloc *a = begin();
    for (Int i = 0; i < LEN(readBytesTests); i++) {
        BytesBuffer *buf = bytes_new_buffer_string(a, readBytesTests[i].buffer);
        Error err = BURROW_NO_ERROR;
        for (Int j = 0; j < expected_count(i); j++) {
            Str s = bytes_buffer_read_string(buf, a, readBytesTests[i].delim, &err);
            if (!str_eq(s, want_line(i, j)))
                testing_t_errorf_v(t, "expected %q, got %q", want_line(i, j), s);
            if (s.len > 0)
                mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
            if (BURROW_FAILED(err))
                break;
        }
        if (!errors_is(err, readBytesTests[i].eof ? io_eof : BURROW_NO_ERROR))
            testing_t_errorf_v(t, "expected error %v, got %v",
                               readBytesTests[i].eof ? io_eof : BURROW_NO_ERROR, err);
        bytes_buffer_free(buf);
    }
    end(t);
}

static void TestPeek(TestingT *t) {
    static const struct {
        const char *buffer;
        Int skip;
        Int n;
        const char *expected;
        bool eof;
    } peekTests[] = {
        {"", 0, 0, "", false},
        {"aaa", 0, 3, "aaa", false},
        {"foobar", 0, 2, "fo", false},
        {"a", 0, 2, "a", true},
        {"helloworld", 4, 3, "owo", false},
        {"helloworld", 5, 5, "world", false},
        {"helloworld", 5, 6, "world", true},
        {"helloworld", 10, 1, "", true},
    };
    Alloc *a = begin();
    for (Int i = 0; i < LEN(peekTests); i++) {
        Str buffer = str_from_cstr(peekTests[i].buffer);
        BytesBuffer *buf = bytes_new_buffer_string(a, buffer);
        bytes_buffer_next(buf, peekTests[i].skip);
        Error err;
        Slice bytes = bytes_buffer_peek(buf, peekTests[i].n, &err);
        Str expected = str_from_cstr(peekTests[i].expected);
        if (!str_eq(sv(bytes), expected))
            testing_t_errorf_v(t, "expected %q, got %q", expected, sv(bytes));
        Error want_err = peekTests[i].eof ? io_eof : BURROW_NO_ERROR;
        if (!errors_is(err, want_err))
            testing_t_errorf_v(t, "expected error %v, got %v", want_err, err);
        if (bytes_buffer_len(buf) != buffer.len - peekTests[i].skip)
            testing_t_errorf_v(t, "bad length after peek: %d, want %d",
                               bytes_buffer_len(buf), buffer.len - peekTests[i].skip);
        bytes_buffer_free(buf);
    }
    end(t);
}

static void TestGrow(TestingT *t) {
    Alloc *a = begin();
    Byte tmp[72];
    static const Int lens[] = {0, 100, 1000, 10000, 100000};
    for (Int gi = 0; gi < LEN(lens); gi++) {
        Int grow_len = lens[gi];
        for (Int si = 0; si < LEN(lens); si++) {
            Int start_len = lens[si];
            Slice x_bytes = bytes_repeat(a, BURROW_B("x"), start_len);

            BytesBuffer *buf = bytes_new_buffer(a, x_bytes);
            /* If we read, this affects buf.off, which is good to test. */
            Int read_bytes =
                bytes_buffer_read(buf, slice_from(tmp, 72, 72, TYPE_BYTE), NULL);
            Slice y_bytes = bytes_repeat(a, BURROW_B("y"), grow_len);
            /* testing.AllocsPerRun(100, ...): one warm up run, then the
             * average of a hundred. */
            bytes_buffer_grow(buf, grow_len);
            bytes_buffer_write(buf, y_bytes, NULL);
            uint64_t before = track.allocs;
            for (int run = 0; run < 100; run++) {
                bytes_buffer_grow(buf, grow_len);
                bytes_buffer_write(buf, y_bytes, NULL);
            }
            /* Check no allocation occurs in write, as long as we're
             * single-threaded. */
            if ((track.allocs - before) / 100 != 0)
                testing_t_errorf_v(t, "allocation occurred during write");
            /* Check that buffer has correct data. */
            Slice got = bytes_buffer_bytes(buf);
            if (!bytes_equal(slice_sub(got, 0, start_len - read_bytes),
                             slice_sub(x_bytes, read_bytes, x_bytes.len)))
                testing_t_errorf_v(t, "bad initial data at %d %d", start_len, grow_len);
            if (!bytes_equal(slice_sub(got, start_len - read_bytes,
                                       start_len - read_bytes + grow_len),
                             y_bytes))
                testing_t_errorf_v(t, "bad written data at %d %d", start_len, grow_len);
            bytes_buffer_free(buf);
            if (x_bytes.cap > 0)
                mem_free(a, x_bytes.p, (size_t)x_bytes.cap, 1);
            if (y_bytes.cap > 0)
                mem_free(a, y_bytes.p, (size_t)y_bytes.cap, 1);
        }
    }
    end(t);
}

static void TestGrowOverflow(TestingT *t) {
    Alloc *a = begin();
    Byte one[1] = {0};
    BytesBuffer *buf = bytes_new_buffer(a, slice_from(one, 1, 1, TYPE_BYTE));
    if (bytes_buffer_grow(buf, BURROW_INT_MAX))
        testing_t_errorf_v(t, "after too-large Grow, got true; want false");
    if (bytes_buffer_len(buf) != 1 || bytes_buffer_cap(buf) != 1)
        testing_t_errorf_v(t, "a failed Grow changed the buffer: len %d cap %d",
                           bytes_buffer_len(buf), bytes_buffer_cap(buf));
    Error err;
    bytes_buffer_write(buf, slice_from(one, 1, 1, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || bytes_buffer_len(buf) != 2)
        testing_t_errorf_v(t, "Write after a failed Grow: %v, len %d", err,
                           bytes_buffer_len(buf));
    bytes_buffer_free(buf);
    end(t);
}

/* Was a bug: used to give EOF reading empty slice at EOF. */
static void TestReadEmptyAtEOF(TestingT *t) {
    BytesBuffer b = BYTES_BUFFER(NULL);
    Byte none[1];
    Error err;
    Int n = bytes_buffer_read(&b, slice_from(none, 0, 0, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "read error: %v", err);
    if (n != 0)
        testing_t_errorf_v(t, "wrong count; got %d want 0", n);
}

static void TestUnreadByte(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer b = BYTES_BUFFER(a);

    /* check at EOF */
    if (BURROW_OK(bytes_buffer_unread_byte(&b)))
        testing_t_fatal_v(t, "UnreadByte at EOF: got no error");
    Error err;
    bytes_buffer_read_byte(&b, &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "ReadByte at EOF: got no error");
    if (BURROW_OK(bytes_buffer_unread_byte(&b)))
        testing_t_fatal_v(t, "UnreadByte after ReadByte at EOF: got no error");

    /* check not at EOF */
    bytes_buffer_write_string(&b, S("abcdefghijklmnopqrstuvwxyz"), NULL);

    /* after unsuccessful read */
    Int n = bytes_buffer_read(&b, slice_nil(TYPE_BYTE), &err);
    if (n != 0 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Read(nil) = %d,%v; want 0,nil", n, err);
    if (BURROW_OK(bytes_buffer_unread_byte(&b)))
        testing_t_fatal_v(t, "UnreadByte after Read(nil): got no error");

    /* after successful read */
    Slice line = bytes_buffer_read_bytes(&b, a, 'm', &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadBytes: %v", err);
    mem_free(a, line.p, (size_t)line.cap, 1);
    err = bytes_buffer_unread_byte(&b);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "UnreadByte: %v", err);
    Byte c = bytes_buffer_read_byte(&b, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadByte: %v", err);
    if (c != 'm')
        testing_t_errorf_v(t, "ReadByte = %q; want %q", c, 'm');
    bytes_buffer_free(&b);
    end(t);
}

/* Tests that we occasionally compact. Issue 5154. */
static void TestBufferGrowth(TestingT *t) {
    Alloc *a = begin();
    BytesBuffer b = BYTES_BUFFER(a);
    Slice buf = make_bytes(1024);
    bytes_buffer_write(&b, slice_sub(buf, 0, 1), NULL);
    Int cap0 = 0;
    for (Int i = 0; i < 5 << 10; i++) {
        bytes_buffer_write(&b, buf, NULL);
        bytes_buffer_read(&b, buf, NULL);
        if (i == 0)
            cap0 = bytes_buffer_cap(&b);
    }
    Int cap1 = bytes_buffer_cap(&b);
    /* (*Buffer).grow allows for 2x capacity slop before sliding, so set our
     * error threshold at 3x. */
    if (cap1 > cap0 * 3)
        testing_t_errorf_v(t, "buffer cap = %d; too big (grew from %d)", cap1, cap0);
    bytes_buffer_free(&b);
    end(t);
}

/* ------------------------------------------------------------ not from Go */

/* A buffer that cannot grow reports it from the write and keeps its contents,
 * where Go would panic with ErrTooLarge. */
static void TestWriteOutOfMemory(TestingT *t) {
    static unsigned char mem[256];
    Fixed fx;
    fixed_init(&fx, mem, sizeof mem);
    BytesBuffer b = BYTES_BUFFER(fixed_allocator(&fx));
    Error err;
    bytes_buffer_write_string(&b, S("hello"), &err);
    CHECK(BURROW_OK(err));
    Slice big = make_bytes(4096);
    Int n = bytes_buffer_write(&b, big, &err);
    CHECK(n == 0 && errors_is(err, burrow_err_out_of_memory));
    CHECK(str_eq(sv(bytes_buffer_bytes(&b)), S("hello")));
    CHECK(!bytes_buffer_grow(&b, 4096));
    CHECK(errors_is(bytes_buffer_write_byte(&b, '!'), BURROW_NO_ERROR));
    CHECK(str_eq(sv(bytes_buffer_bytes(&b)), S("hello!")));
    CHECK(str_eq(error_text(bytes_err_too_large), S("bytes.Buffer: too large")));
}

/* The buffer never frees a slice it was given, and frees what it grew into. */
static void TestNewBufferKeepsCallerSlice(TestingT *t) {
    Alloc *a = begin();
    Byte mine[4] = {'a', 'b', 'c', 'd'};
    BytesBuffer *b = bytes_new_buffer(a, slice_from(mine, 4, 4, TYPE_BYTE));
    bytes_buffer_write_string(b, S("efgh"), NULL);
    CHECK(str_eq(sv(bytes_buffer_bytes(b)), S("abcdefgh")));
    CHECK(mine[0] == 'a');
    CHECK(bytes_buffer_as_io_writer(b).vt->self_type == TYPE_BYTES_BUFFER);
    bytes_buffer_free(b);
    end(t);
}

#define TESTS(X)                                                                       \
    X(TestNewBuffer)                                                                   \
    X(TestNewBufferString)                                                             \
    X(TestBasicOperations)                                                             \
    X(TestLargeStringWrites)                                                           \
    X(TestLargeByteWrites)                                                             \
    X(TestLargeStringReads)                                                            \
    X(TestLargeByteReads)                                                              \
    X(TestMixedReadsAndWrites)                                                         \
    X(TestCapWithPreallocatedSlice)                                                    \
    X(TestCapWithSliceAndWrittenData)                                                  \
    X(TestNil)                                                                         \
    X(TestReadFrom)                                                                    \
    X(TestReadFromPanicReader)                                                         \
    X(TestReadFromNegativeReader)                                                      \
    X(TestWriteTo)                                                                     \
    X(TestWriteAppend)                                                                 \
    X(TestRuneIO)                                                                      \
    X(TestWriteInvalidRune)                                                            \
    X(TestNext)                                                                        \
    X(TestReadBytes)                                                                   \
    X(TestReadString)                                                                  \
    X(TestPeek)                                                                        \
    X(TestGrow)                                                                        \
    X(TestGrowOverflow)                                                                \
    X(TestReadEmptyAtEOF)                                                              \
    X(TestUnreadByte)                                                                  \
    X(TestBufferGrowth)                                                                \
    X(TestWriteOutOfMemory)                                                            \
    X(TestNewBufferKeepsCallerSlice)

TESTING_MAIN(TESTS)
