/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "harness.h"

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

/* --------------------------------------------------------- a reader to test
 *
 * Hands out at most chunk bytes at a time, which is what a socket does and what
 * every bug in a reading loop needs in order to show up. eof_with_last decides
 * whether the end arrives with the last bytes or on the call after them, since
 * Go allows both and a caller has to handle both. */
typedef struct Chunks {
    Str data;
    Int pos;
    Int chunk;
    bool eof_with_last;
    Error fail_with; /* returned instead of the end, when set */
    Int reads;
} Chunks;

static const Type chunks_type = {
    {(const Byte *)"Chunks", 6},
    {(const Byte *)"iotest", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(Chunks),
    (uint16_t)_Alignof(Chunks),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63686e6bU,
    NULL,
};

static Int chunks_read(void *self, Slice p, Error *err) {
    Chunks *c = (Chunks *)self;
    Int left, n, i;

    c->reads++;
    left = c->data.len - c->pos;
    if (left <= 0) {
        *err = BURROW_FAILED(c->fail_with) ? c->fail_with : io_eof;
        return 0;
    }

    n = left < p.len ? left : p.len;
    if (n > c->chunk)
        n = c->chunk;
    for (i = 0; i < n; i++)
        *(Byte *)slice_at(p, i) = c->data.p[c->pos + i];
    c->pos += n;

    if (c->eof_with_last && c->pos == c->data.len)
        *err = BURROW_FAILED(c->fail_with) ? c->fail_with : io_eof;
    return n;
}

static const IoReaderVT chunks_reader_vt = {&chunks_type, chunks_read};

static IoReader chunks_as_io_reader(Chunks *c) {
    IoReader r = {&chunks_reader_vt, c};
    return r;
}

/* ---------------------------------------------------------- writers to test */

typedef struct Sink {
    Byte buf[256];
    Int len;
    Int limit;       /* write no more than this many bytes per call, 0 for all */
    Int lie_by;      /* report this many more bytes than were written */
    Error fail_with; /* fail instead of writing, when set */
    Int writes;
} Sink;

static const Type sink_type = {
    {(const Byte *)"Sink", 4},
    {(const Byte *)"iotest", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(Sink),
    (uint16_t)_Alignof(Sink),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73696e6bU,
    NULL,
};

static Int sink_write(void *self, Slice p, Error *err) {
    Sink *s = (Sink *)self;
    Int n = p.len, i;

    s->writes++;
    if (BURROW_FAILED(s->fail_with)) {
        *err = s->fail_with;
        return 0;
    }
    if (s->limit > 0 && n > s->limit)
        n = s->limit;
    if (n > (Int)sizeof(s->buf) - s->len)
        n = (Int)sizeof(s->buf) - s->len;
    for (i = 0; i < n; i++)
        s->buf[s->len + i] = *(const Byte *)slice_at(p, i);
    s->len += n;
    return n + s->lie_by;
}

static const IoWriterVT sink_writer_vt = {&sink_type, sink_write};

static IoWriter sink_as_io_writer(Sink *s) {
    IoWriter w = {&sink_writer_vt, s};
    return w;
}

static bool sink_holds(const Sink *s, const char *want) {
    Str got = {s->buf, s->len};
    return str_eq(got, str_from_cstr(want));
}

/* A type that is both, so that the down conversions have something real to
 * narrow. This is the shape every ported type will have. */
typedef struct Pipe {
    Chunks in;
    Sink out;
} Pipe;

static Int pipe_read(void *self, Slice p, Error *err) {
    return chunks_read(&((Pipe *)self)->in, p, err);
}

static Int pipe_write(void *self, Slice p, Error *err) {
    return sink_write(&((Pipe *)self)->out, p, err);
}

static const Type pipe_type = {
    {(const Byte *)"Pipe", 4},
    {(const Byte *)"iotest", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(Pipe),
    (uint16_t)_Alignof(Pipe),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x70697065U,
    NULL,
};

static const IoReadWriterVT pipe_read_writer_vt = {
    {&pipe_type, pipe_read},
    {&pipe_type, pipe_write},
};

static IoReadWriter pipe_as_io_read_writer(Pipe *p) {
    IoReadWriter rw = {&pipe_read_writer_vt, p};
    return rw;
}

/* An error that wraps the end of the input, for the one rule that is easy to
 * get wrong: a wrapped io_eof is a failure and not a clean end. */
typedef struct WrappedEof {
    Str text;
} WrappedEof;

static Str wrapped_eof_message(const void *self) {
    return ((const WrappedEof *)self)->text;
}

static Error wrapped_eof_unwrap(const void *self) {
    (void)self;
    return io_eof;
}

static const Type wrapped_eof_type = {
    {(const Byte *)"WrappedEof", 10},
    {(const Byte *)"iotest", 6},
    KIND_STRUCT,
    (uint32_t)sizeof(WrappedEof),
    (uint16_t)_Alignof(WrappedEof),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x77656f66U,
    NULL,
};

static const ErrorVT wrapped_eof_vt = {
    &wrapped_eof_type, wrapped_eof_message, wrapped_eof_unwrap, NULL, NULL, NULL, NULL,
};

static const WrappedEof wrapped_eof_value = {{(const Byte *)"read tcp: EOF", 13}};
static const Error wrapped_eof = {&wrapped_eof_vt, &wrapped_eof_value};

static Str src_text(void) {
    return str_from_cstr("the quick brown fox");
}

/* ----------------------------------------------------------- read_full */

TEST(read_full_fills_the_buffer_however_small_the_reads_are) {
    Chunks c = {{NULL, 0}, 0, 3, false, BURROW_NO_ERROR, 0};
    Byte got[19];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n;

    c.data = src_text();
    n = io_read_full(chunks_as_io_reader(&c), buf, &err);

    CHECK_INT_EQ(n, 19);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(str_from_bytes(got, n), src_text()));
    /* Three bytes at a time, so it had to come back for more. */
    CHECK(c.reads > 1);
}

TEST(read_full_of_an_empty_input_is_the_end_and_not_a_failure) {
    Chunks c = {{(const Byte *)"", 0}, 0, 8, false, BURROW_NO_ERROR, 0};
    Byte got[4];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(chunks_as_io_reader(&c), buf, &err);

    CHECK_INT_EQ(n, 0);
    CHECK(errors_is(err, io_eof));
}

TEST(read_full_of_a_truncated_input_says_unexpected) {
    /* The distinction the function exists for. Nothing at all means there was
     * no next record, and half of one means the file is cut short. */
    Chunks c = {{(const Byte *)"abc", 3}, 0, 8, false, BURROW_NO_ERROR, 0};
    Byte got[8];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(chunks_as_io_reader(&c), buf, &err);

    CHECK_INT_EQ(n, 3);
    CHECK(errors_is(err, io_err_unexpected_eof));
    CHECK(!errors_is(err, io_eof));
}

TEST(read_full_succeeds_when_the_end_arrives_with_the_last_bytes) {
    /* A reader is allowed to report the end in the same call that hands over
     * the last of the data, and that has to be a success. */
    Chunks c = {{(const Byte *)"abcd", 4}, 0, 8, true, BURROW_NO_ERROR, 0};
    Byte got[4];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(chunks_as_io_reader(&c), buf, &err);

    CHECK_INT_EQ(n, 4);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(c.reads, 1);
}

TEST(read_full_passes_a_real_error_through) {
    Chunks c = {{(const Byte *)"ab", 2}, 0, 8, false, errors_err_unsupported, 0};
    Byte got[8];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(chunks_as_io_reader(&c), buf, &err);

    CHECK_INT_EQ(n, 2);
    CHECK(errors_is(err, errors_err_unsupported));
}

TEST(read_full_takes_null_for_the_error) {
    Chunks c = {{(const Byte *)"abcd", 4}, 0, 8, false, BURROW_NO_ERROR, 0};
    Byte got[4];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);

    CHECK_INT_EQ(io_read_full(chunks_as_io_reader(&c), buf, NULL), 4);
}

/* --------------------------------------------------------- read_at_least */

TEST(read_at_least_stops_once_it_has_the_minimum) {
    Chunks c = {{NULL, 0}, 0, 4, false, BURROW_NO_ERROR, 0};
    Byte got[19];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n;

    c.data = src_text();
    n = io_read_at_least(chunks_as_io_reader(&c), buf, 5, &err);

    CHECK(BURROW_OK(err));
    /* At least the five asked for, and it keeps whatever the read that crossed
     * the line handed over, because throwing those away would mean reading them
     * again. */
    CHECK(n >= 5);
    CHECK(n <= 8);
}

TEST(read_at_least_of_more_than_the_buffer_holds_is_a_short_buffer) {
    Chunks c = {{NULL, 0}, 0, 8, false, BURROW_NO_ERROR, 0};
    Byte got[4];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n;

    c.data = src_text();
    n = io_read_at_least(chunks_as_io_reader(&c), buf, 5, &err);

    CHECK_INT_EQ(n, 0);
    CHECK(errors_is(err, io_err_short_buffer));
    /* And it reads nothing, rather than reading and then complaining. */
    CHECK_INT_EQ(c.reads, 0);
}

TEST(read_at_least_of_nothing_reads_nothing) {
    Chunks c = {{NULL, 0}, 0, 8, false, BURROW_NO_ERROR, 0};
    Byte got[4];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int n;

    c.data = src_text();
    n = io_read_at_least(chunks_as_io_reader(&c), buf, 0, &err);

    CHECK_INT_EQ(n, 0);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(c.reads, 0);
}

/* ---------------------------------------------------------------- copy */

TEST(copy_moves_everything_and_counts_it) {
    Chunks c = {{NULL, 0}, 0, 5, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Error err = BURROW_NO_ERROR;
    int64_t n;

    c.data = src_text();
    n = io_copy(a, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(n, 19);
    CHECK(sink_holds(&s, "the quick brown fox"));
}

TEST(copy_of_an_empty_source_is_a_success_that_copies_nothing) {
    Chunks c = {{(const Byte *)"", 0}, 0, 8, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(a, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK_INT_EQ(n, 0);
    CHECK(BURROW_OK(err));
    /* Nothing to write, so nothing was written. A copy that calls Write with an
     * empty buffer would truncate a file that treats a zero length write as a
     * request to do something. */
    CHECK_INT_EQ(s.writes, 0);
}

TEST(copy_does_not_report_the_end_of_the_input_as_an_error) {
    /* The one rule that makes io.Copy usable: the end is where it stops, not
     * something that went wrong. Both shapes of reader, to be sure. */
    Chunks together = {{(const Byte *)"abc", 3}, 0, 8, true, BURROW_NO_ERROR, 0};
    Chunks after = {{(const Byte *)"abc", 3}, 0, 8, false, BURROW_NO_ERROR, 0};
    Sink s1 = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Sink s2 = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Error e1 = BURROW_NO_ERROR, e2 = BURROW_NO_ERROR;

    CHECK_INT_EQ(
        io_copy(a, sink_as_io_writer(&s1), chunks_as_io_reader(&together), &e1), 3);
    CHECK_INT_EQ(io_copy(a, sink_as_io_writer(&s2), chunks_as_io_reader(&after), &e2),
                 3);
    CHECK(BURROW_OK(e1));
    CHECK(BURROW_OK(e2));
    CHECK(sink_holds(&s1, "abc"));
    CHECK(sink_holds(&s2, "abc"));
}

TEST(copy_treats_a_wrapped_end_as_the_failure_it_is) {
    /* Go compares against EOF with == here and this does the same. A reader
     * that wraps the end has taken the one value meaning nothing went wrong and
     * dressed it up as something that did, and believing it would turn a
     * truncated download into a complete one. */
    Chunks c = {{(const Byte *)"abc", 3}, 0, 8, false, wrapped_eof, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(a, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK_INT_EQ(n, 3);
    CHECK(BURROW_FAILED(err));
    CHECK(str_eq(error_text(err), str_from_cstr("read tcp: EOF")));
    /* It does unwrap to the end, which is how a caller that wants to be
     * generous can still ask. */
    CHECK(errors_is(err, io_eof));
}

TEST(copy_stops_at_a_read_error_and_keeps_what_it_already_wrote) {
    Chunks c = {{(const Byte *)"abcdef", 6}, 0, 3, false, errors_err_unsupported, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(a, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK_INT_EQ(n, 6);
    CHECK(errors_is(err, errors_err_unsupported));
    CHECK(sink_holds(&s, "abcdef"));
}

TEST(copy_stops_at_a_write_error) {
    Chunks c = {{NULL, 0}, 0, 4, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 0, errors_err_unsupported, 0};
    Error err = BURROW_NO_ERROR;
    int64_t n;

    c.data = src_text();
    n = io_copy(a, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK_INT_EQ(n, 0);
    CHECK(errors_is(err, errors_err_unsupported));
    CHECK_INT_EQ(s.writes, 1);
}

TEST(copy_catches_a_writer_that_writes_less_than_it_was_given) {
    Chunks c = {{(const Byte *)"abcdef", 6}, 0, 6, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 2, 0, BURROW_NO_ERROR, 0}; /* two bytes a call, no error */
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(a, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK_INT_EQ(n, 2);
    CHECK(errors_is(err, io_err_short_write));
    CHECK(sink_holds(&s, "ab"));
}

TEST(copy_catches_a_writer_that_claims_more_than_it_was_given) {
    /* A Writer that has lost count. Believing the number would mean reporting a
     * copy longer than the input, so the count is thrown away and the message
     * says whose fault it is. */
    Chunks c = {{(const Byte *)"abcdef", 6}, 0, 6, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 3, BURROW_NO_ERROR, 0};
    Error err = BURROW_NO_ERROR;
    int64_t n = io_copy(a, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK_INT_EQ(n, 0);
    CHECK(BURROW_FAILED(err));
    CHECK(str_eq(error_text(err), str_from_cstr("invalid write result")));
}

TEST(copy_through_a_buffer_uses_the_buffer_it_was_given) {
    Chunks c = {{NULL, 0}, 0, 64, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Byte small[4];
    Slice buf = slice_from(small, (Int)sizeof small, (Int)sizeof small, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    int64_t n;

    c.data = src_text();
    n = io_copy_buffer(sink_as_io_writer(&s), chunks_as_io_reader(&c), buf, &err);

    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(n, 19);
    CHECK(sink_holds(&s, "the quick brown fox"));
    /* Four bytes at a time out of a nineteen byte source, so five writes and
     * not one. The buffer is doing the work it was handed. */
    CHECK_INT_EQ(s.writes, 5);
}

TEST(copy_through_an_empty_buffer_is_a_short_buffer_and_not_a_hang) {
    Chunks c = {{NULL, 0}, 0, 8, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Slice empty = slice_nil(TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    int64_t n;

    c.data = src_text();
    n = io_copy_buffer(sink_as_io_writer(&s), chunks_as_io_reader(&c), empty, &err);

    CHECK_INT_EQ(n, 0);
    CHECK(errors_is(err, io_err_short_buffer));
    CHECK_INT_EQ(c.reads, 0);
}

TEST(copy_reports_an_allocator_that_cannot_give_it_a_buffer) {
    Chunks c = {{NULL, 0}, 0, 8, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Fixed fx;
    Alloc *small;
    static Byte tiny[64];
    Error err = BURROW_NO_ERROR;
    int64_t n;

    c.data = src_text();
    fixed_init(&fx, tiny, sizeof tiny);
    small = fixed_allocator(&fx);
    n = io_copy(small, sink_as_io_writer(&s), chunks_as_io_reader(&c), &err);

    CHECK_INT_EQ(n, 0);
    CHECK(errors_is(err, burrow_err_out_of_memory));
    CHECK_INT_EQ(c.reads, 0);
}

TEST(copy_gives_the_buffer_back_when_it_is_done) {
    /* Twice through an arena that can hold one buffer and not two. The second
     * copy proves the first one freed what it borrowed. */
    Chunks c1 = {{(const Byte *)"abc", 3}, 0, 8, false, BURROW_NO_ERROR, 0};
    Chunks c2 = {{(const Byte *)"def", 3}, 0, 8, false, BURROW_NO_ERROR, 0};
    Sink s = {{0}, 0, 0, 0, BURROW_NO_ERROR, 0};
    Fixed fx;
    Alloc *one_buffer;
    static Byte room[32 * 1024 + 64];
    Error err = BURROW_NO_ERROR;

    fixed_init(&fx, room, sizeof room);
    one_buffer = fixed_allocator(&fx);

    CHECK_INT_EQ(
        io_copy(one_buffer, sink_as_io_writer(&s), chunks_as_io_reader(&c1), &err), 3);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(
        io_copy(one_buffer, sink_as_io_writer(&s), chunks_as_io_reader(&c2), &err), 3);
    CHECK(BURROW_OK(err));
    CHECK(sink_holds(&s, "abcdef"));
}

/* ---------------------------------------------------------- the conversions */

TEST(a_read_writer_narrows_to_a_reader_and_to_a_writer) {
    Pipe p;
    IoReadWriter rw;
    IoReader r;
    IoWriter w;
    Byte got[3];
    Slice buf = slice_from(got, (Int)sizeof got, (Int)sizeof got, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;

    memset(&p, 0, sizeof p);
    p.in.data = str_from_cstr("abc");
    p.in.chunk = 8;

    rw = pipe_as_io_read_writer(&p);
    r = io_read_writer_as_io_reader(rw);
    w = io_read_writer_as_io_writer(rw);

    /* Both halves point at the same object and at their own part of the one
     * vtable, which is the whole reason the vtables are members. */
    CHECK(r.data == &p);
    CHECK(w.data == &p);
    CHECK((const void *)r.vt == (const void *)&pipe_read_writer_vt.reader);
    CHECK((const void *)w.vt == (const void *)&pipe_read_writer_vt.writer);

    CHECK_INT_EQ(io_read_full(r, buf, &err), 3);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(BURROW_CALL(w, write, buf, &err), 3);
    CHECK(BURROW_OK(err));
    CHECK(sink_holds(&p.out, "abc"));
}

TEST(narrowing_a_nil_value_gives_a_nil_value) {
    IoReadWriter rw = {NULL, NULL};
    IoReadCloser rc = {NULL, NULL};
    IoWriteCloser wc = {NULL, NULL};
    IoReadWriteCloser rwc = {NULL, NULL};

    CHECK(BURROW_IFACE_IS_NIL(io_read_writer_as_io_reader(rw)));
    CHECK(BURROW_IFACE_IS_NIL(io_read_writer_as_io_writer(rw)));
    CHECK(BURROW_IFACE_IS_NIL(io_read_closer_as_io_reader(rc)));
    CHECK(BURROW_IFACE_IS_NIL(io_read_closer_as_io_closer(rc)));
    CHECK(BURROW_IFACE_IS_NIL(io_write_closer_as_io_writer(wc)));
    CHECK(BURROW_IFACE_IS_NIL(io_write_closer_as_io_closer(wc)));
    CHECK(BURROW_IFACE_IS_NIL(io_read_write_closer_as_io_reader(rwc)));
    CHECK(BURROW_IFACE_IS_NIL(io_read_write_closer_as_io_writer(rwc)));
    CHECK(BURROW_IFACE_IS_NIL(io_read_write_closer_as_io_closer(rwc)));
}

TEST(a_reader_keeps_its_type_through_an_interface) {
    Chunks c = {{(const Byte *)"abc", 3}, 0, 8, false, BURROW_NO_ERROR, 0};
    IoReader r = chunks_as_io_reader(&c);

    CHECK(iface_type(BURROW_IFACE(r)) == &chunks_type);
    CHECK(iface_assert(BURROW_IFACE(r), &chunks_type) == &c);
    CHECK(iface_assert(BURROW_IFACE(r), &sink_type) == NULL);
}

/* ---------------------------------------------------------------- sentinels */

TEST(the_sentinels_carry_gos_messages) {
    CHECK(str_eq(error_text(io_eof), str_from_cstr("EOF")));
    CHECK(str_eq(error_text(io_err_unexpected_eof), str_from_cstr("unexpected EOF")));
    CHECK(str_eq(error_text(io_err_short_write), str_from_cstr("short write")));
    CHECK(str_eq(error_text(io_err_short_buffer), str_from_cstr("short buffer")));
    CHECK(str_eq(error_text(io_err_no_progress),
                 str_from_cstr("multiple Read calls return no data or error")));
}

TEST(the_sentinels_are_distinct_from_each_other) {
    CHECK(!errors_is(io_eof, io_err_unexpected_eof));
    CHECK(!errors_is(io_err_short_write, io_err_short_buffer));
    CHECK(errors_is(io_eof, io_eof));
}

int main(void) {
    setup();
    RUN(read_full_fills_the_buffer_however_small_the_reads_are);
    RUN(read_full_of_an_empty_input_is_the_end_and_not_a_failure);
    RUN(read_full_of_a_truncated_input_says_unexpected);
    RUN(read_full_succeeds_when_the_end_arrives_with_the_last_bytes);
    RUN(read_full_passes_a_real_error_through);
    RUN(read_full_takes_null_for_the_error);
    RUN(read_at_least_stops_once_it_has_the_minimum);
    RUN(read_at_least_of_more_than_the_buffer_holds_is_a_short_buffer);
    RUN(read_at_least_of_nothing_reads_nothing);
    RUN(copy_moves_everything_and_counts_it);
    RUN(copy_of_an_empty_source_is_a_success_that_copies_nothing);
    RUN(copy_does_not_report_the_end_of_the_input_as_an_error);
    RUN(copy_treats_a_wrapped_end_as_the_failure_it_is);
    RUN(copy_stops_at_a_read_error_and_keeps_what_it_already_wrote);
    RUN(copy_stops_at_a_write_error);
    RUN(copy_catches_a_writer_that_writes_less_than_it_was_given);
    RUN(copy_catches_a_writer_that_claims_more_than_it_was_given);
    RUN(copy_through_a_buffer_uses_the_buffer_it_was_given);
    RUN(copy_through_an_empty_buffer_is_a_short_buffer_and_not_a_hang);
    RUN(copy_reports_an_allocator_that_cannot_give_it_a_buffer);
    RUN(copy_gives_the_buffer_back_when_it_is_done);
    RUN(a_read_writer_narrows_to_a_reader_and_to_a_writer);
    RUN(narrowing_a_nil_value_gives_a_nil_value);
    RUN(a_reader_keeps_its_type_through_an_interface);
    RUN(the_sentinels_carry_gos_messages);
    RUN(the_sentinels_are_distinct_from_each_other);
    teardown();
    return harness_report("io");
}
