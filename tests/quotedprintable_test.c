/* Derived from Go's src/mime/quotedprintable/reader_test.go and
 * writer_test.go. Go source: go1.27.1.
 *
 * tests/quotedprintable_test_gen.h, from tools/gen-quotedprintable-tests.sh,
 * holds what Go's reader and writer make of Go's own test inputs and of more,
 * so each result here is checked against Go's byte for byte. Go's
 * TestExhaustive compares against the qprint program when it is installed and
 * otherwise only checks which errors come up. Here every string of up to six
 * bytes from a slightly larger alphabet goes through the reader, and a digest
 * of all the results has to match the one Go's reader gives.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime/quotedprintable.h"

#include "quotedprintable_test_gen.h"

#include <string.h>

static Str str_of(const char *p, long long n) {
    return (Str){(const Byte *)p, (Int)n};
}

static bool str_is(Str s, const char *want, long long n) {
    return s.len == (Int)n && (n == 0 || memcmp(s.p, want, (size_t)n) == 0);
}

static bool text_is(Str s, const char *want) {
    return str_is(s, want, (long long)strlen(want));
}

static Str err_text(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : (Str){NULL, 0};
}

static Str bytes_str(Slice b) {
    return (Str){(const Byte *)b.p, b.len};
}

/* Reads everything the way Go's test does, with io.Copy. */
static Str read_all(Alloc *a, Str in, Error *err) {
    StringsReader sr;
    strings_reader_reset(&sr, in);
    QuotedprintableReader *r =
        quotedprintable_new_reader(a, strings_reader_as_io_reader(&sr));
    BytesBuffer out = BYTES_BUFFER(a);
    io_copy(a, bytes_buffer_as_io_writer(&out), quotedprintable_reader_as_io_reader(r),
            err);
    quotedprintable_reader_free(r);
    return bytes_str(bytes_buffer_bytes(&out));
}

/* One byte at a time, up to the first error, io_eof included. */
static Str read_bytes(Alloc *a, Str in, Error *err) {
    StringsReader sr;
    strings_reader_reset(&sr, in);
    QuotedprintableReader *r =
        quotedprintable_new_reader(a, strings_reader_as_io_reader(&sr));
    BytesBuffer out = BYTES_BUFFER(a);
    Byte c;
    for (;;) {
        Int n = quotedprintable_reader_read(r, slice_from(&c, 1, 1, TYPE_BYTE), err);
        if (n > 0)
            bytes_buffer_write_byte(&out, c);
        if (errors_is(*err, io_eof)) {
            *err = BURROW_NO_ERROR;
            break;
        }
        if (BURROW_FAILED(*err))
            break;
    }
    quotedprintable_reader_free(r);
    return bytes_str(bytes_buffer_bytes(&out));
}

static uint64_t fnv_add(uint64_t h, Str s) {
    Byte l[8];
    for (int i = 0; i < 8; i++)
        l[i] = (Byte)((uint64_t)s.len >> (8 * i));
    for (int i = 0; i < 8; i++) {
        h ^= l[i];
        h *= 1099511628211ULL;
    }
    for (Int i = 0; i < s.len; i++) {
        h ^= s.p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* The inputs of qp_long_cases, as the generator writes them. */
static Str long_input(Alloc *a, size_t i) {
    BytesBuffer b = BYTES_BUFFER(a);
    Error err;
    if (i == 0) {
        for (int k = 0; k < 5000; k++)
            bytes_buffer_write_byte(&b, 'x');
        bytes_buffer_write_byte(&b, '\n');
        for (int k = 0; k < 10; k++)
            bytes_buffer_write_byte(&b, 'y');
    } else {
        for (int k = 0; k < 2000; k++)
            bytes_buffer_write_string(&b, BURROW_S("=41"), &err);
        bytes_buffer_write_string(&b, BURROW_S("=\r\n"), &err);
        for (int k = 0; k < 3000; k++)
            bytes_buffer_write_string(&b, BURROW_S("z "), &err);
        bytes_buffer_write_string(&b, BURROW_S("\r\n"), &err);
    }
    return bytes_str(bytes_buffer_bytes(&b));
}

static void TestReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof qp_read_cases / sizeof qp_read_cases[0]; i++) {
        const QpReadCase *c = &qp_read_cases[i];
        Str in = str_of(c->in, c->in_len);
        Error err;
        Str got = read_all(a, in, &err);
        if (!str_is(got, c->want, c->want_len))
            testing_t_errorf_v(t, "for %q, got %q; want %q", in, got,
                               str_of(c->want, c->want_len));
        if (!text_is(err_text(err), c->err))
            testing_t_errorf_v(t, "for %q, got error %q; want %q", in, err_text(err),
                               c->err);
        got = read_bytes(a, in, &err);
        if (!str_is(got, c->want1, c->want1_len))
            testing_t_errorf_v(t, "for %q a byte at a time, got %q; want %q", in, got,
                               str_of(c->want1, c->want1_len));
        if (!text_is(err_text(err), c->err1))
            testing_t_errorf_v(t, "for %q a byte at a time, got error %q; want %q", in,
                               err_text(err), c->err1);
        arena_reset(&ar);
    }
    for (size_t i = 0; i < sizeof qp_long_cases / sizeof qp_long_cases[0]; i++) {
        const QpLongCase *c = &qp_long_cases[i];
        Str in = long_input(a, i);
        Error err;
        Str got = read_all(a, in, &err);
        if (got.len != (Int)c->want_len ||
            fnv_add(14695981039346656037ULL, got) != c->want_fnv)
            testing_t_errorf_v(t, "long input %d: got %d bytes, want %d", (int)i,
                               (int)got.len, (int)c->want_len);
        if (!text_is(err_text(err), c->err))
            testing_t_errorf_v(t, "long input %d: got error %q; want %q", (int)i,
                               err_text(err), c->err);
        got = read_bytes(a, in, &err);
        if (got.len != (Int)c->want1_len ||
            fnv_add(14695981039346656037ULL, got) != c->want1_fnv)
            testing_t_errorf_v(t,
                               "long input %d a byte at a time: got %d bytes, want %d",
                               (int)i, (int)got.len, (int)c->want1_len);
        if (!text_is(err_text(err), c->err1))
            testing_t_errorf_v(t,
                               "long input %d a byte at a time: got error %q; want %q",
                               (int)i, err_text(err), c->err1);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestExhaustive(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const char *alpha = QP_EXHAUSTIVE_ALPHABET;
    int na = (int)strlen(alpha);
    uint64_t h = 14695981039346656037ULL;
    long long count = 0;
    for (int n = 0; n <= QP_EXHAUSTIVE_MAX; n++) {
        int idx[QP_EXHAUSTIVE_MAX] = {0};
        Byte b[QP_EXHAUSTIVE_MAX];
        for (;;) {
            for (int i = 0; i < n; i++)
                b[i] = (Byte)alpha[idx[i]];
            Str in = {b, n};
            Error err;
            Str got = read_all(a, in, &err);
            h = fnv_add(h, in);
            h = fnv_add(h, got);
            h = fnv_add(h, err_text(err));
            count++;
            arena_reset(&ar);
            int i = n - 1;
            while (i >= 0) {
                if (++idx[i] < na)
                    break;
                idx[i] = 0;
                i--;
            }
            if (i < 0)
                break;
        }
    }
    CHECK(count == QP_EXHAUSTIVE_COUNT);
    if (h != QP_EXHAUSTIVE_FNV)
        testing_t_errorf_v(t, "digest of %d decodes is %x, want %x", (int)count, h,
                           (uint64_t)QP_EXHAUSTIVE_FNV);
    arena_free(&ar);
}

/* The same bytes as rndText in the generator. */
static Str rnd_text(Alloc *a, uint64_t seed, Int n) {
    static const char pick[] =
        "abcdefghijklmnopqrstuvwxyz        \t\t\r\n\r\n==.~!\0\177\200"
        "\303\251\377";
    Byte *b = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    uint64_t x = seed;
    for (Int i = 0; i < n; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        b[i] = (Byte)pick[(x >> 33) % (sizeof pick - 1)];
    }
    return (Str){b, n};
}

static Str encode(Alloc *a, Str in, bool binary, Int piece) {
    BytesBuffer out = BYTES_BUFFER(a);
    QuotedprintableWriter *w =
        quotedprintable_new_writer(a, bytes_buffer_as_io_writer(&out));
    w->binary = binary;
    if (piece == 0)
        piece = in.len + 1;
    for (Int off = 0; off < in.len; off += piece) {
        Int k = in.len - off < piece ? in.len - off : piece;
        Error err;
        Int n = quotedprintable_writer_write(
            w, slice_from((void *)(uintptr_t)(in.p + off), k, k, TYPE_BYTE), &err);
        if (n != k || BURROW_FAILED(err))
            return (Str){NULL, -1};
    }
    if (BURROW_FAILED(quotedprintable_writer_close(w)))
        return (Str){NULL, -1};
    quotedprintable_writer_free(w);
    return bytes_str(bytes_buffer_bytes(&out));
}

static void TestWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof qp_write_cases / sizeof qp_write_cases[0]; i++) {
        const QpWriteCase *c = &qp_write_cases[i];
        Str in = c->rnd_seed ? rnd_text(a, (uint64_t)c->rnd_seed, (Int)c->in_len)
                             : str_of(c->in, c->in_len);
        struct {
            bool binary;
            Int piece;
            const char *want;
        } runs[] = {{false, 0, c->text},
                    {false, 3, c->text3},
                    {true, 0, c->binary},
                    {true, 3, c->binary3}};
        for (size_t k = 0; k < 4; k++) {
            Str got = encode(a, in, runs[k].binary, runs[k].piece);
            if (!text_is(got, runs[k].want))
                testing_t_errorf_v(
                    t, "Write(%q) binary %t in pieces of %d, got:\n%q\nwant:\n%q", in,
                    runs[k].binary, (int)runs[k].piece, got, runs[k].want);
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* Go's TestRoundTrip, on every byte value as binary and on the random text. */
static void TestRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte all[512];
    for (int i = 0; i < 512; i++)
        all[i] = (Byte)(i * 7);
    Str ins[] = {{all, 512}, rnd_text(a, 99, 5000)};
    for (size_t i = 0; i < 2; i++) {
        Str enc = encode(a, ins[i], true, 0);
        Error err;
        Str dec = read_all(a, enc, &err);
        CHECK(BURROW_OK(err));
        CHECK(str_is(dec, (const char *)ins[i].p, ins[i].len));
    }
    arena_free(&ar);
}

/* A writer that takes limit bytes and then fails. */
typedef struct Limited {
    Int limit;
} Limited;

static const Str limited_err_text = {(const Byte *)"limited: full", 13};
static const Error limited_err = {&burrow_sentinel_error_vt, &limited_err_text};

static Int limited_write(void *self, Slice p, Error *err) {
    Limited *l = (Limited *)self;
    if (p.len > l->limit) {
        *err = limited_err;
        return 0;
    }
    l->limit -= p.len;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT limited_vt = {NULL, limited_write};

static void TestWriterErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str in = rnd_text(a, 7, 1000);
    Str full = encode(a, in, false, 0);
    for (Int limit = 0; limit < full.len; limit += 37) {
        Limited l = {limit};
        QuotedprintableWriter *w =
            quotedprintable_new_writer(a, (IoWriter){&limited_vt, &l});
        Error err;
        Int n = quotedprintable_writer_write(
            w, slice_from((void *)(uintptr_t)in.p, in.len, in.len, TYPE_BYTE), &err);
        if (BURROW_OK(err))
            err = quotedprintable_writer_close(w);
        CHECK(errors_is(err, limited_err));
        CHECK(n >= 0 && n <= in.len);
        quotedprintable_writer_free(w);
    }
    arena_free(&ar);
}

/* A reader that is already a big enough bufio.Reader is read directly, and
 * freeing the quoted-printable reader leaves it to its owner. */
static void TestReaderOverBufio(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("caf=C3=A9=\r\n au lait\r\n"));
    BufioReader *br =
        bufio_new_reader(heap_allocator(), strings_reader_as_io_reader(&sr));
    IoReader src = {NULL, NULL};
    src = bufio_reader_as_io_reader(br);
    QuotedprintableReader *r = quotedprintable_new_reader(a, src);
    Error err;
    Slice got = io_read_all(a, quotedprintable_reader_as_io_reader(r), &err);
    CHECK(BURROW_OK(err));
    CHECK(text_is(bytes_str(got), "caf\303\251 au lait\r\n"));
    quotedprintable_reader_free(r);
    CHECK(bufio_reader_buffered(br) == 0);
    bufio_reader_free(br);
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

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (p == NULL)
        return;
    b->live -= (long long)size;
    mem_free(heap_allocator(), p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, NULL, budget_free, NULL, NULL};

static void TestNoMemory(TestingT *t) {
    int made = 0;
    for (int budget = 0; budget < 5; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        StringsReader sr;
        strings_reader_reset(&sr, BURROW_S("a=3Db"));
        QuotedprintableReader *r =
            quotedprintable_new_reader(&al, strings_reader_as_io_reader(&sr));
        if (r != NULL) {
            Byte buf[8];
            Error err;
            Int n =
                quotedprintable_reader_read(r, slice_from(buf, 8, 8, TYPE_BYTE), &err);
            CHECK(n == 3 && memcmp(buf, "a=b", 3) == 0);
            quotedprintable_reader_free(r);
            made++;
        }
        Budget bw = {budget, 0};
        Alloc alw = {&budget_vt, &bw, NULL, NULL};
        Limited l = {1000};
        QuotedprintableWriter *w =
            quotedprintable_new_writer(&alw, (IoWriter){&limited_vt, &l});
        if (w != NULL) {
            CHECK(budget >= 1);
            quotedprintable_writer_free(w);
        }
        if (b.live != 0 || bw.live != 0)
            testing_t_errorf_v(t, "budget %d: %d bytes leaked", budget,
                               (int)(b.live + bw.live));
    }
    CHECK(made > 0);
}

static void TestInterfaces(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer out = BYTES_BUFFER(a);
    QuotedprintableWriter *w =
        quotedprintable_new_writer(a, bytes_buffer_as_io_writer(&out));
    IoWriteCloser wc = quotedprintable_writer_as_io_write_closer(w);
    Error err;
    io_write_string(quotedprintable_writer_as_io_writer(w), BURROW_S("x = 1 "), &err);
    CHECK(BURROW_OK(err));
    CHECK(BURROW_OK(wc.vt->closer.close(wc.data)));
    CHECK(text_is(bytes_str(bytes_buffer_bytes(&out)), "x =3D 1=20"));
    CHECK(wc.vt->writer.self_type == TYPE_QUOTEDPRINTABLE_WRITER);
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("x"));
    QuotedprintableReader *r =
        quotedprintable_new_reader(a, strings_reader_as_io_reader(&sr));
    CHECK(quotedprintable_reader_as_io_reader(r).vt->self_type ==
          TYPE_QUOTEDPRINTABLE_READER);
    quotedprintable_reader_free(r);
    quotedprintable_writer_free(w);
    quotedprintable_reader_free(NULL);
    quotedprintable_writer_free(NULL);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReader)                                                                      \
    X(TestExhaustive)                                                                  \
    X(TestWriter)                                                                      \
    X(TestRoundTrip)                                                                   \
    X(TestWriterErrors)                                                                \
    X(TestReaderOverBufio)                                                             \
    X(TestNoMemory)                                                                    \
    X(TestInterfaces)

TESTING_MAIN(TESTS)
