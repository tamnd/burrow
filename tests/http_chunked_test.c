/* Derived from Go's src/net/http/internal/chunked_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing/iotest.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static void write_str(HttpChunkedWriter *w, Str s) {
    Error err;
    (void)burrow__http_chunked_writer_write(w, bytes_of(s), &err);
}

static void TestChunk(TestingT *t) {
    ARENA_BEGIN;
    BytesBuffer b = BYTES_BUFFER(a);

    HttpChunkedWriter w = {bytes_buffer_as_io_writer(&b), NULL};
    Str chunk1 = S("hello, ");
    Str chunk2 = S("world! 0123456789abcdef");
    write_str(&w, chunk1);
    write_str(&w, chunk2);
    (void)burrow__http_chunked_writer_close(&w);

    Str g = str_of(bytes_buffer_bytes(&b));
    Str e = S("7\r\nhello, \r\n17\r\nworld! 0123456789abcdef\r\n0\r\n");
    if (!str_eq(g, e))
        testing_t_fatalf_v(t, "chunk writer wrote %q; want %q", g, e);

    HttpChunkedReader *r =
        burrow__http_new_chunked_reader(a, bytes_buffer_as_io_reader(&b));
    Error err;
    Slice data = io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err)) {
        testing_t_logf_v(t, "data: \"%s\"", str_of(data));
        testing_t_fatalf_v(t, "ReadAll from reader: %v", err);
    }
    Str want = S("hello, world! 0123456789abcdef");
    if (!str_eq(str_of(data), want))
        testing_t_errorf_v(t, "chunk reader read %q; want %q", str_of(data), want);
    burrow__http_chunked_reader_free(a, r);
    ARENA_END;
}

static void TestChunkReadMultiple(TestingT *t) {
    ARENA_BEGIN;
    /* Bunch of small chunks, all read together. */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        HttpChunkedWriter w = {bytes_buffer_as_io_writer(&b), NULL};
        write_str(&w, S("foo"));
        write_str(&w, S("bar"));
        (void)burrow__http_chunked_writer_close(&w);

        HttpChunkedReader *r =
            burrow__http_new_chunked_reader(a, bytes_buffer_as_io_reader(&b));
        Byte buf[10];
        Error err;
        Int n = burrow__http_chunked_reader_read(r, slice_from(buf, 10, 10, TYPE_BYTE),
                                                 &err);
        if (n != 6 || !errors_is(err, io_eof))
            testing_t_errorf_v(t, "Read = %d, %v; want 6, EOF", n, err);
        if (!str_eq(str_from_bytes(buf, n), S("foobar")))
            testing_t_errorf_v(t, "Read = %q; want %q", str_from_bytes(buf, n),
                               S("foobar"));
        burrow__http_chunked_reader_free(a, r);
    }

    /* One big chunk followed by a little chunk, but the small bufio reader
     * size should prevent the second chunk header from being read. */
    {
        BytesBuffer b = BYTES_BUFFER(a);
        HttpChunkedWriter w = {bytes_buffer_as_io_writer(&b), NULL};
        /* fillBufChunk is 11 bytes + 3 bytes header + 2 bytes footer = 16
         * bytes, the same as the bufio reader size below (the minimum), so
         * even though we're going to try to Read with a buffer large enough
         * to also receive "foo", the second chunk header won't be read yet. */
        Str fill_buf_chunk = S("0123456789a");
        Str short_chunk = S("foo");
        write_str(&w, fill_buf_chunk);
        write_str(&w, short_chunk);
        (void)burrow__http_chunked_writer_close(&w);

        BufioReader *br = bufio_new_reader_size(a, bytes_buffer_as_io_reader(&b), 16);
        HttpChunkedReader *r =
            burrow__http_new_chunked_reader(a, bufio_reader_as_io_reader(br));
        Byte buf[14];
        Error err;
        Int n = burrow__http_chunked_reader_read(r, slice_from(buf, 14, 14, TYPE_BYTE),
                                                 &err);
        if (n != fill_buf_chunk.len || BURROW_FAILED(err))
            testing_t_errorf_v(t, "Read = %d, %v; want %d, nil", n, err,
                               fill_buf_chunk.len);
        if (!str_eq(str_from_bytes(buf, n), fill_buf_chunk))
            testing_t_errorf_v(t, "Read = %q; want %q", str_from_bytes(buf, n),
                               fill_buf_chunk);

        n = burrow__http_chunked_reader_read(r, slice_from(buf, n, n, TYPE_BYTE), &err);
        if (n != short_chunk.len || !errors_is(err, io_eof))
            testing_t_errorf_v(t, "Read = %d, %v; want %d, EOF", n, err,
                               short_chunk.len);
        burrow__http_chunked_reader_free(a, r);
        bufio_reader_free(br);
    }

    /* And test that we see an EOF chunk, even though our buffer is already
     * full. */
    {
        StringsReader sr;
        strings_reader_reset(&sr, S("3\r\nfoo\r\n0\r\n"));
        BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
        HttpChunkedReader *r =
            burrow__http_new_chunked_reader(a, bufio_reader_as_io_reader(br));
        Byte buf[3];
        Error err;
        Int n =
            burrow__http_chunked_reader_read(r, slice_from(buf, 3, 3, TYPE_BYTE), &err);
        if (n != 3 || !errors_is(err, io_eof))
            testing_t_errorf_v(t, "Read = %d, %v; want 3, EOF", n, err);
        if (memcmp(buf, "foo", 3) != 0)
            testing_t_errorf_v(t, "buf = %q; want foo", str_from_bytes(buf, 3));
        burrow__http_chunked_reader_free(a, r);
        bufio_reader_free(br);
    }
    ARENA_END;
}

/* Go counts the allocations of a run, and allows one. The equivalent here is
 * that each run takes one block from the allocator, the reader itself, since
 * the BufioReader it is given is used as it is. */
static void TestChunkReaderAllocs(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    HttpChunkedWriter w = {bytes_buffer_as_io_writer(&buf), NULL};
    Str sa = S("aaaaaa"), sb = S("bbbbbbbbbbbb"), sc = S("cccccccccccccccccccccccc");
    write_str(&w, sa);
    write_str(&w, sb);
    write_str(&w, sc);
    (void)burrow__http_chunked_writer_close(&w);

    Int want = sa.len + sb.len + sc.len;
    Byte read_buf[43];
    BytesReader byter;
    bytes_reader_reset(&byter, bytes_buffer_bytes(&buf));
    BufioReader *bufr = bufio_new_reader(a, bytes_reader_as_io_reader(&byter));
    uint64_t mallocs = 0;
    for (int i = 0; i < 100; i++) {
        Arena run;
        arena_init(&run, NULL, 0);
        Alloc *ra = arena_allocator(&run);
        AllocStats before = mem_stats(ra);
        (void)bytes_reader_seek(&byter, 0, BURROW_IO_SEEK_START, NULL);
        bufio_reader_reset(bufr, bytes_reader_as_io_reader(&byter));
        HttpChunkedReader *r =
            burrow__http_new_chunked_reader(ra, bufio_reader_as_io_reader(bufr));
        Error err;
        Int n = io_read_full(burrow__http_chunked_reader_as_io_reader(r),
                             slice_from(read_buf, want + 1, want + 1, TYPE_BYTE), &err);
        if (n != want)
            testing_t_fatalf_v(t, "read %d bytes; want %d", n, want);
        if (!errors_is(err, io_err_unexpected_eof))
            testing_t_fatalf_v(t, "read error = %v; want ErrUnexpectedEOF", err);
        burrow__http_chunked_reader_free(ra, r);
        mallocs += mem_stats(ra).allocs - before.allocs;
        arena_free(&run);
    }
    if (mallocs != 100)
        testing_t_errorf_v(t, "mallocs = %d; want 100", (Int)mallocs);
    bufio_reader_free(bufr);
    ARENA_END;
}

typedef struct HexCase {
    const char *in;
    uint64_t want;
    const char *want_err;
} HexCase;

static void TestParseHexUint(TestingT *t) {
    static const HexCase tests[] = {
        {"x", 0, "invalid byte in chunk length"},
        {"0000000000000000", 0, NULL},
        {"0000000000000001", 1, NULL},
        {"ffffffffffffffff", UINT64_MAX, NULL},
        {"000000000000bogus", 0, "invalid byte in chunk length"},
        {"00000000000000000", 0,
         "http chunk length too large"}, /* could accept if we wanted */
        {"10000000000000000", 0, "http chunk length too large"},
        {"00000000000000001", 0,
         "http chunk length too large"}, /* could accept if we wanted */
        {"", 0, "empty hex number for chunk length"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const HexCase *tt = &tests[i];
        Str in = str_from_cstr(tt->in);
        Error err;
        uint64_t got = burrow__http_parse_hex_uint(bytes_of(in), &err);
        if (tt->want_err != NULL) {
            if (!BURROW_FAILED(err) ||
                !strings_contains(error_text(err), str_from_cstr(tt->want_err)))
                testing_t_errorf_v(t, "parseHexUint(%q) = %d, %v; want error %q", in,
                                   (Int)got, err, str_from_cstr(tt->want_err));
        } else if (BURROW_FAILED(err) || got != tt->want) {
            testing_t_errorf_v(t, "parseHexUint(%q) = %v, %v; want %v", in, got, err,
                               tt->want);
        }
    }
    for (uint64_t i = 0; i <= 1234; i++) {
        Byte b[20];
        Slice in = strconv_append_uint(NULL, slice_from(b, 0, 20, TYPE_BYTE), i, 16);
        Error err;
        uint64_t got = burrow__http_parse_hex_uint(in, &err);
        if (BURROW_FAILED(err) || got != i)
            testing_t_errorf_v(t, "parseHexUint(%q) = %v, %v; want %v", str_of(in), got,
                               err, i);
    }
}

static void TestChunkReadingIgnoresExtensions(TestingT *t) {
    ARENA_BEGIN;
    Str in = S("7;ext=\"some quoted string\"\r\n" /* token=quoted string */
               "hello, \r\n"
               "17;someext\r\n" /* token without value */
               "world! 0123456789abcdef\r\n"
               "0;someextension=sometoken\r\n"); /* token=token */
    StringsReader sr;
    strings_reader_reset(&sr, in);
    HttpChunkedReader *r =
        burrow__http_new_chunked_reader(a, strings_reader_as_io_reader(&sr));
    Error err;
    Slice data = io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadAll = %q, %v", str_of(data), err);
    Str e = S("hello, world! 0123456789abcdef");
    if (!str_eq(str_of(data), e))
        testing_t_errorf_v(t, "read %q; want %q", str_of(data), e);
    burrow__http_chunked_reader_free(a, r);
    ARENA_END;
}

typedef struct PipeJob {
    IoPipeWriter *w;
    Str data;
} PipeJob;

static void pipe_write(void *env) {
    PipeJob *j = (PipeJob *)env;
    Error err;
    (void)io_pipe_writer_write(j->w, bytes_of(j->data), &err);
}

/* Issue 17355: the chunked reader shouldn't block waiting for more data if it
 * can return something. */
static void TestChunkReadPartial(TestingT *t) {
    Alloc *a = heap_allocator();
    IoPipeReader *pr;
    IoPipeWriter *pw;
    io_pipe(a, &pr, &pw);
    SyncWaitGroup wg = {0};
    PipeJob j1 = {pw, S("7\r\n1234567")};
    sync_wait_group_go(&wg, BURROW_FN(Func, pipe_write, &j1));
    HttpChunkedReader *cr =
        burrow__http_new_chunked_reader(a, io_pipe_reader_as_io_reader(pr));
    Byte read_buf[7];
    Error err;
    Int n = burrow__http_chunked_reader_read(cr, slice_from(read_buf, 7, 7, TYPE_BYTE),
                                             &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (n != 7 || memcmp(read_buf, "1234567", 7) != 0)
        testing_t_errorf_v(t, "Read: %v %q; want %d, %q", n,
                           str_from_bytes(read_buf, n), (Int)7, S("1234567"));
    sync_wait_group_wait(&wg);
    if (!testing_t_failed(t)) {
        PipeJob j2 = {pw, S("xx")};
        sync_wait_group_go(&wg, BURROW_FN(Func, pipe_write, &j2));
        (void)burrow__http_chunked_reader_read(
            cr, slice_from(read_buf, 7, 7, TYPE_BYTE), &err);
        Str got = error_text(err);
        if (!strings_contains(got, S("malformed")))
            testing_t_errorf_v(t, "second read = %v; want malformed error", err);
        sync_wait_group_wait(&wg);
    }
    (void)io_pipe_reader_close(pr);
    (void)io_pipe_writer_close(pw);
    burrow__http_chunked_reader_free(a, cr);
    io_pipe_free(pr);
}

/* Issue 48861: the chunked reader should report incomplete chunks. */
static void TestIncompleteChunk(TestingT *t) {
    ARENA_BEGIN;
    Str valid = S("4\r\nabcd\r\n"
                  "5\r\nabc\r\n\r\n"
                  "0\r\n");

    for (Int i = 0; i < valid.len; i++) {
        Str incomplete = str_from_bytes(valid.p, i);
        StringsReader sr;
        strings_reader_reset(&sr, incomplete);
        HttpChunkedReader *r =
            burrow__http_new_chunked_reader(a, strings_reader_as_io_reader(&sr));
        Error err;
        (void)io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
        if (!errors_is(err, io_err_unexpected_eof))
            testing_t_errorf_v(t, "expected io.ErrUnexpectedEOF for %q, got %v",
                               incomplete, err);
        burrow__http_chunked_reader_free(a, r);
    }

    StringsReader sr;
    strings_reader_reset(&sr, valid);
    HttpChunkedReader *r =
        burrow__http_new_chunked_reader(a, strings_reader_as_io_reader(&sr));
    Error err;
    (void)io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error for %q: %v", valid, err);
    burrow__http_chunked_reader_free(a, r);
    ARENA_END;
}

BURROW_SENTINEL_ERROR(chunk_end_read_error, "chunk end read error");

static void TestChunkEndReadError(TestingT *t) {
    ARENA_BEGIN;
    StringsReader sr;
    strings_reader_reset(&sr, S("4\r\nabcd"));
    IoReader parts[2] = {strings_reader_as_io_reader(&sr),
                         iotest_err_reader(a, chunk_end_read_error)};
    HttpChunkedReader *r =
        burrow__http_new_chunked_reader(a, io_multi_reader(a, parts, 2));
    Error err;
    (void)io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
    if (err.vt != chunk_end_read_error.vt || err.data != chunk_end_read_error.data)
        testing_t_errorf_v(t, "expected %v, got %v", chunk_end_read_error, err);
    burrow__http_chunked_reader_free(a, r);
    ARENA_END;
}

/* Go's funcReader: it hands out what f gives for each call in turn. Here f is
 * a chunk for the first bodylen calls and the last chunk after that. */
typedef struct FuncReader {
    Str chunk;
    Int bodylen;
    Int i;
    Str b;
} FuncReader;

static Int func_reader_read(void *self, Slice p, Error *err) {
    FuncReader *r = (FuncReader *)self;
    if (r->b.len == 0) {
        r->b = r->i < r->bodylen ? r->chunk : S("0\r\n");
        r->i++;
    }
    Int n = p.len < r->b.len ? p.len : r->b.len;
    memcpy(p.p, r->b.p, (size_t)n);
    r->b = str_from_bytes(r->b.p + n, r->b.len - n);
    *err = BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT func_reader_vt = {NULL, func_reader_read};

static void TestChunkReaderTooMuchOverhead(TestingT *t) {
    ARENA_BEGIN;
    /* If the sender is sending 100x as many chunk header bytes as chunk data,
     * we should reject the stream at some point. */
    Byte chunk[107];
    memcpy(chunk, "1;", 2);
    memset(chunk + 2, 'a', 100); /* chunk extension */
    memcpy(chunk + 102, "\r\nX\r\n", 5);
    FuncReader fr = {str_from_bytes(chunk, 107), 1 << 20, 0, {0}};
    HttpChunkedReader *r =
        burrow__http_new_chunked_reader(a, (IoReader){&func_reader_vt, &fr});
    Error err;
    (void)io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(
            t, "successfully read body with excessive overhead; want error");
    burrow__http_chunked_reader_free(a, r);
    ARENA_END;
}

static void TestChunkReaderByteAtATime(TestingT *t) {
    ARENA_BEGIN;
    /* Sending one byte per chunk should not trip the excess-overhead
     * detection. */
    Int bodylen = 1 << 20;
    FuncReader fr = {S("1\r\nX\r\n"), bodylen, 0, {0}};
    HttpChunkedReader *r =
        burrow__http_new_chunked_reader(a, (IoReader){&func_reader_vt, &fr});
    Error err;
    Slice got = io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error: %v", err);
    if (got.len != bodylen)
        testing_t_errorf_v(t, "read %v bytes, want %v", got.len, bodylen);
    burrow__http_chunked_reader_free(a, r);
    ARENA_END;
}

static void chunk_invalid_input(void *env, TestingT *t) {
    ARENA_BEGIN;
    Str b = *(const Str *)env;
    StringsReader sr;
    strings_reader_reset(&sr, b);
    HttpChunkedReader *r =
        burrow__http_new_chunked_reader(a, strings_reader_as_io_reader(&sr));
    Error err;
    Slice got = io_read_all(a, burrow__http_chunked_reader_as_io_reader(r), &err);
    burrow__http_chunked_reader_free(a, r);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpectedly parsed invalid chunked data:\n%q",
                           str_of(got));
    ARENA_END;
}

static void TestChunkInvalidInputs(TestingT *t) {
    static const struct {
        Str name;
        Str b;
    } tests[] = {
        {BURROW_S_INIT("bare LF in chunk size"), BURROW_S_INIT("1\na\r\n0\r\n")},
        {BURROW_S_INIT("extra LF in chunk size"), BURROW_S_INIT("1\r\r\na\r\n0\r\n")},
        {BURROW_S_INIT("bare LF in chunk data"), BURROW_S_INIT("1\r\na\n0\r\n")},
        {BURROW_S_INIT("bare LF in chunk extension"), BURROW_S_INIT("1;\na\r\n0\r\n")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(t, tests[i].name,
                      BURROW_FN(TestingTFunc, chunk_invalid_input,
                                (void *)(uintptr_t)&tests[i].b));
}

/* Not Go's: a writer with flush set flushes the BufioWriter after each chunk,
 * which is what FlushAfterChunkWriter is for, and a write of no bytes writes
 * nothing. */
static void TestChunkFlushAfterChunk(TestingT *t) {
    ARENA_BEGIN;
    BytesBuffer b = BYTES_BUFFER(a);
    BufioWriter *bw = bufio_new_writer(a, bytes_buffer_as_io_writer(&b));
    HttpChunkedWriter w = {bufio_writer_as_io_writer(bw), bw};
    Error err;
    Int n =
        burrow__http_chunked_writer_write(&w, slice_from(NULL, 0, 0, TYPE_BYTE), &err);
    if (n != 0 || BURROW_FAILED(err) || bytes_buffer_len(&b) != 0)
        testing_t_errorf_v(t, "empty write = %d, %v, wrote %d bytes", n, err,
                           bytes_buffer_len(&b));
    n = burrow__http_chunked_writer_write(&w, bytes_of(S("hello")), &err);
    if (n != 5 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write = %d, %v; want 5, nil", n, err);
    Str want = S("5\r\nhello\r\n");
    if (!str_eq(str_of(bytes_buffer_bytes(&b)), want))
        testing_t_errorf_v(t, "after one chunk the buffer has %q; want %q",
                           str_of(bytes_buffer_bytes(&b)), want);
    bufio_writer_free(bw);
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestChunk)                                                                       \
    X(TestChunkReadMultiple)                                                           \
    X(TestChunkReaderAllocs)                                                           \
    X(TestParseHexUint)                                                                \
    X(TestChunkReadingIgnoresExtensions)                                               \
    X(TestChunkReadPartial)                                                            \
    X(TestIncompleteChunk)                                                             \
    X(TestChunkEndReadError)                                                           \
    X(TestChunkReaderTooMuchOverhead)                                                  \
    X(TestChunkReaderByteAtATime)                                                      \
    X(TestChunkInvalidInputs)                                                          \
    X(TestChunkFlushAfterChunk)

TESTING_MAIN(TESTS)
