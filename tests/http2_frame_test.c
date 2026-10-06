/* Derived from net/http/internal/http2's frame_test.go and errors_test.go in
 * Go 1.27.1.
 *
 * Go's tests compare whole frames with reflect.DeepEqual, and these compare
 * the header and each field of the kind of frame. Every frame read is freed,
 * except where Go's test is about frames staying apart, and there they are
 * kept until the end.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http2.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

/* A scratch arena for the strings in failure messages, freed when the test
 * ends. */
typedef struct H2Scratch {
    Arena ar;
    Alloc *a;
} H2Scratch;

static void h2_scratch_free(void *env) {
    arena_free(&((H2Scratch *)env)->ar);
    mem_free(heap_allocator(), env, sizeof(H2Scratch), _Alignof(H2Scratch));
}

static Alloc *ta(TestingT *t) {
    H2Scratch *e =
        (H2Scratch *)mem_alloc(heap_allocator(), sizeof *e, _Alignof(H2Scratch));
    if (e == NULL)
        panic_str(BURROW_S("out of memory"));
    arena_init(&e->ar, NULL, 0);
    e->a = arena_allocator(&e->ar);
    testing_t_cleanup(t, BURROW_FN(Func, h2_scratch_free, e));
    return e->a;
}

/* testFramer: one buffer that the Framer writes to and reads from. */
typedef struct TF {
    BytesBuffer buf;
    Http2Framer *fr;
} TF;

static Http2Framer *tf_init(TF *tf) {
    tf->buf = BYTES_BUFFER(heap_allocator());
    tf->fr =
        burrow__http2_new_framer(heap_allocator(), bytes_buffer_as_io_writer(&tf->buf),
                                 bytes_buffer_as_io_reader(&tf->buf));
    if (tf->fr == NULL)
        panic_str(BURROW_S("out of memory"));
    return tf->fr;
}

static void tf_free(TF *tf) {
    burrow__http2_framer_free(tf->fr);
    bytes_buffer_free(&tf->buf);
}

static Slice bs_n(const char *s, Int n) {
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static Slice bs(const char *s) {
    return bs_n(s, (Int)strlen(s));
}

static bool slice_is(Slice got, const char *want, Int n) {
    return got.len == n && (n == 0 || memcmp(got.p, want, (size_t)n) == 0);
}

static bool slice_eq(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

/* buf.String() == want, for a want that has NULs in it. */
#define BUF_IS(tf, s) slice_is(bytes_buffer_bytes(&(tf)->buf), (s), (Int)sizeof(s) - 1)

static bool header_eq(Http2FrameHeader a, Http2FrameHeader b) {
    return a.valid == b.valid && a.type == b.type && a.flags == b.flags &&
           a.length == b.length && a.stream_id == b.stream_id;
}

static Http2FrameHeader fhdr(Http2FrameType type, Http2Flags flags, uint32_t length,
                             uint32_t stream_id) {
    Http2FrameHeader h = {0};
    h.valid = true;
    h.type = type;
    h.flags = flags;
    h.length = length;
    h.stream_id = stream_id;
    return h;
}

static Str header_string(Alloc *a, Http2FrameHeader h) {
    return burrow__http2_frame_header_string(a, h);
}

static Http2PriorityParam prio(uint32_t dep, bool exclusive, uint8_t weight) {
    Http2PriorityParam p = {0};
    p.stream_dep = dep;
    p.exclusive = exclusive;
    p.weight = weight;
    return p;
}

static Http2PriorityParam rfc9218(uint8_t urgency, uint8_t incremental) {
    Http2PriorityParam p = {0};
    p.urgency = urgency;
    p.incremental = incremental;
    return p;
}

static bool is_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool is_connection_error(Error err, Http2ErrCode code) {
    Http2ErrCode got = 0;
    return burrow__http2_error_connection(err, &got) && got == code;
}

/* ------------------------------------------------------- errors_test.go */

static void TestErrCodeString(TestingT *t) {
    static const struct {
        Http2ErrCode err;
        const char *want;
    } tests[] = {
        {HTTP2_ERR_CODE_PROTOCOL, "PROTOCOL_ERROR"},
        {0xd, "HTTP_1_1_REQUIRED"},
        {0xf, "unknown error code 0xf"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Byte buf[HTTP2_STRING_MAX];
        Str got = burrow__http2_err_code_string(tests[i].err, buf);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%d. Error = %q; want %q", (int)i, got,
                               str_from_cstr(tests[i].want));
    }
}

/* -------------------------------------------------------- frame_test.go */

static void TestFrameSizes(TestingT *t) {
    /* Catch people rearranging the FrameHeader fields. */
    if (sizeof(Http2FrameHeader) != 12)
        testing_t_errorf_v(t, "FrameHeader size = %d; want %d",
                           (int)sizeof(Http2FrameHeader), 12);
}

static void TestFrameTypeString(TestingT *t) {
    static const struct {
        Http2FrameType ft;
        const char *want;
    } tests[] = {
        {HTTP2_FRAME_DATA, "DATA"},
        {HTTP2_FRAME_PING, "PING"},
        {HTTP2_FRAME_GO_AWAY, "GOAWAY"},
        {0x20, "UNKNOWN_FRAME_TYPE_32"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Byte buf[HTTP2_STRING_MAX];
        Str got = burrow__http2_frame_type_string(tests[i].ft, buf);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%d. String(FrameType %d) = %q; want %q", (int)i,
                               (int)tests[i].ft, got, str_from_cstr(tests[i].want));
    }
}

static void TestWriteRST(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    uint32_t stream_id = (1U << 24) + (2U << 16) + (3U << 8) + 4U;
    uint32_t err_code = (7U << 24) + (6U << 16) + (5U << 8) + 4U;
    (void)burrow__http2_framer_write_rst_stream(fr, stream_id, err_code);
    if (!BUF_IS(&tf, "\x00\x00\x04\x03\x00\x01\x02\x03\x04\x07\x06\x05\x04"))
        testing_t_errorf_v(t, "encoded as %q", bytes_buffer_bytes(&tf.buf));
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (f->kind != HTTP2_RST_STREAM_FRAME ||
        !header_eq(f->header, fhdr(0x3, 0, 4, 0x1020304)) ||
        f->u.rst_stream.err_code != 0x7060504)
        testing_t_errorf_v(t, "parsed back %s, ErrCode %d",
                           header_string(ta(t), f->header), f->u.rst_stream.err_code);
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

static void TestWriteData(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    uint32_t stream_id = (1U << 24) + (2U << 16) + (3U << 8) + 4U;
    Slice data = bs("ABC");
    (void)burrow__http2_framer_write_data(fr, stream_id, true, data);
    if (!BUF_IS(&tf, "\x00\x00\x03\x00\x01\x01\x02\x03\x04"
                     "ABC"))
        testing_t_errorf_v(t, "encoded as %q", bytes_buffer_bytes(&tf.buf));
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (f->kind != HTTP2_DATA_FRAME) {
        testing_t_fatalf_v(t, "got kind %d; want DataFrame", (int)f->kind);
        burrow__http2_frame_free(f);
        tf_free(&tf);
        return;
    }
    Slice got = burrow__http2_data_frame_data(f);
    if (!slice_eq(got, data))
        testing_t_errorf_v(t, "got %q; want %q", got, data);
    if ((f->header.flags & 1) == 0)
        testing_t_errorf_v(t, "didn't see END_STREAM flag");
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

static void TestWriteDataPadded(TestingT *t) {
    static const Byte zeros[3] = {0};
    struct {
        uint32_t stream_id;
        bool end_stream;
        Slice data;
        Slice pad;
        Http2FrameHeader want_header;
    } tests[3];
    /* Unpadded: */
    tests[0].stream_id = 1;
    tests[0].end_stream = true;
    tests[0].data = bs("foo");
    tests[0].pad = (Slice){0};
    tests[0].want_header = fhdr(HTTP2_FRAME_DATA, HTTP2_FLAG_DATA_END_STREAM, 3, 1);
    /* Padded bit set, but no padding: */
    tests[1].stream_id = 1;
    tests[1].end_stream = true;
    tests[1].data = bs("foo");
    tests[1].pad = bs_n((const char *)zeros, 0);
    tests[1].want_header = fhdr(
        HTTP2_FRAME_DATA, HTTP2_FLAG_DATA_END_STREAM | HTTP2_FLAG_DATA_PADDED, 4, 1);
    /* Padded bit set, with padding: */
    tests[2].stream_id = 1;
    tests[2].end_stream = false;
    tests[2].data = bs("foo");
    tests[2].pad = bs_n((const char *)zeros, 3);
    tests[2].want_header = fhdr(HTTP2_FRAME_DATA, HTTP2_FLAG_DATA_PADDED, 7, 1);

    for (int i = 0; i < 3; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        (void)burrow__http2_framer_write_data_padded(
            fr, tests[i].stream_id, tests[i].end_stream, tests[i].data, tests[i].pad);
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. ReadFrame: %v", i, err);
            tf_free(&tf);
            continue;
        }
        if (!header_eq(f->header, tests[i].want_header)) {
            testing_t_errorf_v(t, "%d. read %s; want %s", i,
                               header_string(ta(t), f->header),
                               header_string(ta(t), tests[i].want_header));
        } else {
            Slice got = burrow__http2_data_frame_data(f);
            if (!slice_eq(got, tests[i].data))
                testing_t_errorf_v(t, "%d. got %q; want %q", i, got, tests[i].data);
        }
        burrow__http2_frame_free(f);
        tf_free(&tf);
    }
}

static void TestWriteHeaders(TestingT *t) {
    static const Http2Flags all_flags =
        HTTP2_FLAG_HEADERS_END_STREAM | HTTP2_FLAG_HEADERS_END_HEADERS |
        HTTP2_FLAG_HEADERS_PADDED | HTTP2_FLAG_HEADERS_PRIORITY;
    struct {
        const char *name;
        Http2HeadersFrameParam p;
        const char *want_enc;
        Int want_enc_len;
        Http2FrameHeader want_header;
        Http2PriorityParam want_priority;
        const char *want_frag;
    } tests[6];
    memset(tests, 0, sizeof tests);

    tests[0].name = "basic";
    tests[0].p.stream_id = 42;
    tests[0].p.block_fragment = bs("abc");
    tests[0].want_enc = "\x00\x00\x03\x01\x00\x00\x00\x00*abc";
    tests[0].want_enc_len = 12;
    tests[0].want_header = fhdr(HTTP2_FRAME_HEADERS, 0, 3, 42);
    tests[0].want_frag = "abc";

    tests[1].name = "basic + end flags";
    tests[1].p.stream_id = 42;
    tests[1].p.block_fragment = bs("abc");
    tests[1].p.end_stream = true;
    tests[1].p.end_headers = true;
    tests[1].want_enc = "\x00\x00\x03\x01\x05\x00\x00\x00*abc";
    tests[1].want_enc_len = 12;
    tests[1].want_header =
        fhdr(HTTP2_FRAME_HEADERS,
             HTTP2_FLAG_HEADERS_END_STREAM | HTTP2_FLAG_HEADERS_END_HEADERS, 3, 42);
    tests[1].want_frag = "abc";

    tests[2].name = "with padding";
    tests[2].p.stream_id = 42;
    tests[2].p.block_fragment = bs("abc");
    tests[2].p.end_stream = true;
    tests[2].p.end_headers = true;
    tests[2].p.pad_length = 5;
    tests[2].want_enc = "\x00\x00\t\x01\r\x00\x00\x00*\x05"
                        "abc\x00\x00\x00\x00\x00";
    tests[2].want_enc_len = 18;
    /* pad length + contents + padding */
    tests[2].want_header =
        fhdr(HTTP2_FRAME_HEADERS,
             HTTP2_FLAG_HEADERS_END_STREAM | HTTP2_FLAG_HEADERS_END_HEADERS |
                 HTTP2_FLAG_HEADERS_PADDED,
             1 + 3 + 5, 42);
    tests[2].want_frag = "abc";

    tests[3].name = "with priority";
    tests[3].p.stream_id = 42;
    tests[3].p.block_fragment = bs("abc");
    tests[3].p.end_stream = true;
    tests[3].p.end_headers = true;
    tests[3].p.pad_length = 2;
    tests[3].p.priority = prio(15, true, 127);
    tests[3].want_enc = "\x00\x00\v\x01-\x00\x00\x00*\x02\x80\x00\x00\x0f\x7f"
                        "abc\x00\x00";
    tests[3].want_enc_len = 20;
    /* pad length + priority + contents + padding */
    tests[3].want_header = fhdr(HTTP2_FRAME_HEADERS, all_flags, 1 + 5 + 3 + 2, 42);
    tests[3].want_priority = prio(15, true, 127);
    tests[3].want_frag = "abc";

    /* golang.org/issue/15444 */
    tests[4].name = "with priority stream dep zero";
    tests[4].p.stream_id = 42;
    tests[4].p.block_fragment = bs("abc");
    tests[4].p.end_stream = true;
    tests[4].p.end_headers = true;
    tests[4].p.pad_length = 2;
    tests[4].p.priority = prio(0, true, 127);
    tests[4].want_enc = "\x00\x00\v\x01-\x00\x00\x00*\x02\x80\x00\x00\x00\x7f"
                        "abc\x00\x00";
    tests[4].want_enc_len = 20;
    tests[4].want_header = fhdr(HTTP2_FRAME_HEADERS, all_flags, 1 + 5 + 3 + 2, 42);
    tests[4].want_priority = prio(0, true, 127);
    tests[4].want_frag = "abc";

    tests[5].name = "zero length";
    tests[5].p.stream_id = 42;
    tests[5].want_enc = "\x00\x00\x00\x01\x00\x00\x00\x00*";
    tests[5].want_enc_len = 9;
    tests[5].want_header = fhdr(HTTP2_FRAME_HEADERS, 0, 0, 42);
    tests[5].want_frag = "";

    for (int i = 0; i < 6; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        Str name = str_from_cstr(tests[i].name);
        Error err = burrow__http2_framer_write_headers(fr, tests[i].p);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: %v", name, err);
            tf_free(&tf);
            continue;
        }
        if (!slice_is(bytes_buffer_bytes(&tf.buf), tests[i].want_enc,
                      tests[i].want_enc_len))
            testing_t_errorf_v(t, "test %q: encoded %q; want %q", name,
                               bytes_buffer_bytes(&tf.buf),
                               bs_n(tests[i].want_enc, tests[i].want_enc_len));
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: failed to read the frame back: %v", name,
                               err);
            tf_free(&tf);
            continue;
        }
        if (f->kind != HTTP2_HEADERS_FRAME ||
            !header_eq(f->header, tests[i].want_header) ||
            !burrow__http2_priority_param_equal(f->u.headers.priority,
                                                tests[i].want_priority) ||
            !slice_eq(burrow__http2_headers_frame_header_block_fragment(f),
                      bs(tests[i].want_frag)))
            testing_t_errorf_v(t, "test %q: mismatch.\n got: %s %q\nwant: %s %q", name,
                               header_string(ta(t), f->header),
                               burrow__http2_headers_frame_header_block_fragment(f),
                               header_string(ta(t), tests[i].want_header),
                               str_from_cstr(tests[i].want_frag));
        burrow__http2_frame_free(f);
        tf_free(&tf);
    }
}

static void TestWriteInvalidStreamDep(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Http2HeadersFrameParam p = {0};
    p.stream_id = 42;
    p.priority.stream_dep = 1U << 31;
    Error err = burrow__http2_framer_write_headers(fr, p);
    if (!is_same_error(err, burrow__http2_err_dep_stream_id))
        testing_t_errorf_v(t, "header error = %v; want %q", err,
                           burrow__http2_err_dep_stream_id);

    err = burrow__http2_framer_write_priority(fr, 2, prio(1U << 31, false, 0));
    if (!is_same_error(err, burrow__http2_err_dep_stream_id))
        testing_t_errorf_v(t, "priority error = %v; want %q", err,
                           burrow__http2_err_dep_stream_id);
    tf_free(&tf);
}

static void TestWriteContinuation(TestingT *t) {
    static const struct {
        const char *name;
        bool end;
        const char *frag;
        Http2Flags want_flags;
    } tests[] = {
        {"not end", false, "abc", 0},
        {"end", true, "def", HTTP2_FLAG_CONTINUATION_END_HEADERS},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        Str name = str_from_cstr(tests[i].name);
        Error err = burrow__http2_framer_write_continuation(fr, 42, tests[i].end,
                                                            bs(tests[i].frag));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: %v", name, err);
            tf_free(&tf);
            continue;
        }
        fr->allow_illegal_reads = true;
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: failed to read the frame back: %v", name,
                               err);
            tf_free(&tf);
            continue;
        }
        Http2FrameHeader want = fhdr(HTTP2_FRAME_CONTINUATION, tests[i].want_flags,
                                     (uint32_t)strlen(tests[i].frag), 42);
        if (f->kind != HTTP2_CONTINUATION_FRAME || !header_eq(f->header, want) ||
            !slice_eq(f->u.continuation.header_frag_buf, bs(tests[i].frag)))
            testing_t_errorf_v(t, "test %q: mismatch.\n got: %s\nwant: %s", name,
                               header_string(ta(t), f->header),
                               header_string(ta(t), want));
        burrow__http2_frame_free(f);
        tf_free(&tf);
    }
}

static void TestParseRFC9218Priority(TestingT *t) {
    struct {
        const char *name;
        const char *priority_str;
        Http2PriorityParam want;
        bool want_ok;
    } tests[] = {
        {"with urgency", "u=0", rfc9218(0, 0), true},
        {"with implicit incremental", "i", rfc9218(3, 1), true},
        {"with explicit incremental", "i=?1", rfc9218(3, 1), true},
        {"with urgency and incremental", "i=?0, u=4", rfc9218(4, 0), true},
        {"with other valid dictionary data",
         "some=data;someparam;u=fake, u=1;foo, i;bar", rfc9218(1, 1), true},
        {"repeated field", "u=1,i,u=5,i=?0", rfc9218(5, 0), true},
        {"wrong field type", "u=\"urgency will be ignored\", i", rfc9218(3, 1), true},
        {"invalid dictionary", "u=1,i, but this is not a valid dictionary\"",
         burrow__http2_default_rfc9218_priority(true), false},
        {"out of range value", "u=8", burrow__http2_default_rfc9218_priority(true),
         true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Http2PriorityParam got;
        bool got_ok = burrow__http2_parse_rfc9218_priority(
            str_from_cstr(tests[i].priority_str), true, &got);
        Str name = str_from_cstr(tests[i].name);
        if (got_ok != tests[i].want_ok)
            testing_t_errorf_v(t, "test %q: got ok %t; want ok %t", name, got_ok,
                               tests[i].want_ok);
        if (!burrow__http2_priority_param_equal(got, tests[i].want))
            testing_t_errorf_v(t,
                               "test %q: mismatch.\n got: u=%d i=%d\nwant: u=%d i=%d",
                               name, got.urgency, got.incremental,
                               tests[i].want.urgency, tests[i].want.incremental);
    }
}

static void TestWritePriority(TestingT *t) {
    struct {
        const char *name;
        Http2PriorityParam priority;
    } tests[] = {
        {"not exclusive", prio(2, false, 127)},
        {"exclusive", prio(3, true, 77)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        Str name = str_from_cstr(tests[i].name);
        Error err = burrow__http2_framer_write_priority(fr, 42, tests[i].priority);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: %v", name, err);
            tf_free(&tf);
            continue;
        }
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: failed to read the frame back: %v", name,
                               err);
            tf_free(&tf);
            continue;
        }
        Http2FrameHeader want = fhdr(HTTP2_FRAME_PRIORITY, 0, 5, 42);
        if (f->kind != HTTP2_PRIORITY_FRAME || !header_eq(f->header, want) ||
            !burrow__http2_priority_param_equal(f->u.priority.priority,
                                                tests[i].priority))
            testing_t_errorf_v(t, "test %q: mismatch.\n got: %s\nwant: %s", name,
                               header_string(ta(t), f->header),
                               header_string(ta(t), want));
        burrow__http2_frame_free(f);
        tf_free(&tf);
    }
}

static void TestWritePriorityUpdate(TestingT *t) {
    static const struct {
        const char *name;
        const char *priority;
        uint32_t want_length;
    } tests[] = {
        {"with urgency", "u=0", 7},
        {"with incremental", "i", 5},
        {"with urgency and incremental", "u=7,i", 9},
        {"with other fields", "a=123,u=7,i,b;a;b", 21},
        {"with string escapes", "u=\"invalid\" , i", 19},
        {"with empty payload", "", 4},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        Str name = str_from_cstr(tests[i].name);
        Str priority = str_from_cstr(tests[i].priority);
        Error err = burrow__http2_framer_write_priority_update(fr, 42, priority);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: %v", name, err);
            tf_free(&tf);
            continue;
        }
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %q: failed to read the frame back: %v", name,
                               err);
            tf_free(&tf);
            continue;
        }
        Http2FrameHeader want =
            fhdr(HTTP2_FRAME_PRIORITY_UPDATE, 0, tests[i].want_length, 0);
        if (f->kind != HTTP2_PRIORITY_UPDATE_FRAME || !header_eq(f->header, want) ||
            !str_eq(f->u.priority_update.priority, priority) ||
            f->u.priority_update.prioritized_stream_id != 42)
            testing_t_errorf_v(t, "test %q: mismatch.\n got: %s %q %d\nwant: %s %q 42",
                               name, header_string(ta(t), f->header),
                               f->u.priority_update.priority,
                               f->u.priority_update.prioritized_stream_id,
                               header_string(ta(t), want), priority);
        burrow__http2_frame_free(f);
        tf_free(&tf);
    }
}

typedef struct SettingsSeen {
    TestingT *t;
    const Http2Frame *sf;
    Http2Setting got[8];
    int n;
} SettingsSeen;

static Error settings_seen(void *env, Http2Setting s) {
    SettingsSeen *st = (SettingsSeen *)env;
    if (st->n < 8)
        st->got[st->n] = s;
    st->n++;
    uint32_t val_back = 0;
    bool ok = burrow__http2_settings_frame_value(st->sf, s.id, &val_back);
    if (!ok || val_back != s.val)
        testing_t_errorf_v(st->t, "Value(%d) = %d, %t; want %d, true", s.id, val_back,
                           ok, s.val);
    return BURROW_NO_ERROR;
}

static void TestWriteSettings(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Http2Setting settings[] = {{1, 2}, {3, 4}};
    (void)burrow__http2_framer_write_settings(fr, settings, 2);
    if (!BUF_IS(&tf,
                "\x00\x00\f\x04\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x02\x00\x03\x00"
                "\x00\x00\x04"))
        testing_t_errorf_v(t, "encoded as %q", bytes_buffer_bytes(&tf.buf));
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (f->kind != HTTP2_SETTINGS_FRAME) {
        testing_t_fatalf_v(t, "Got kind %d; want a SettingsFrame", (int)f->kind);
        burrow__http2_frame_free(f);
        tf_free(&tf);
        return;
    }
    SettingsSeen st = {0};
    st.t = t;
    st.sf = f;
    (void)burrow__http2_settings_frame_foreach_setting(
        f, BURROW_FN(Http2SettingFunc, settings_seen, &st));
    if (st.n != 2 || st.got[0].id != 1 || st.got[0].val != 2 || st.got[1].id != 3 ||
        st.got[1].val != 4)
        testing_t_errorf_v(t, "Read %d settings != written settings", st.n);
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

static void TestWriteSettingsAck(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    (void)burrow__http2_framer_write_settings_ack(fr);
    if (!BUF_IS(&tf, "\x00\x00\x00\x04\x01\x00\x00\x00\x00"))
        testing_t_errorf_v(t, "encoded as %q", bytes_buffer_bytes(&tf.buf));
    tf_free(&tf);
}

static void TestWriteWindowUpdate(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    uint32_t stream_id = (1U << 24) + (2U << 16) + (3U << 8) + 4U;
    uint32_t incr = (7U << 24) + (6U << 16) + (5U << 8) + 4U;
    Error err = burrow__http2_framer_write_window_update(fr, stream_id, incr);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (!BUF_IS(&tf, "\x00\x00\x04\x08\x00\x01\x02\x03\x04\x07\x06\x05\x04"))
        testing_t_errorf_v(t, "encoded as %q", bytes_buffer_bytes(&tf.buf));
    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (f->kind != HTTP2_WINDOW_UPDATE_FRAME ||
        !header_eq(f->header, fhdr(0x8, 0, 4, 0x1020304)) ||
        f->u.window_update.increment != 0x7060504)
        testing_t_errorf_v(t, "parsed back %s, Increment %d",
                           header_string(ta(t), f->header),
                           f->u.window_update.increment);
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

static void test_write_ping(TestingT *t, bool ack) {
    static const Byte data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Error err = burrow__http2_framer_write_ping(fr, ack, data);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    Http2Flags want_flags = ack ? HTTP2_FLAG_PING_ACK : 0;
    Byte want_enc[17] = {0, 0, 8, 6, want_flags, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    if (!slice_is(bytes_buffer_bytes(&tf.buf), (const char *)want_enc, 17))
        testing_t_errorf_v(t, "encoded as %q; want %q", bytes_buffer_bytes(&tf.buf),
                           bs_n((const char *)want_enc, 17));

    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (f->kind != HTTP2_PING_FRAME ||
        !header_eq(f->header, fhdr(0x6, want_flags, 8, 0)) ||
        memcmp(f->u.ping.data, data, 8) != 0)
        testing_t_errorf_v(t, "parsed back %s", header_string(ta(t), f->header));
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

static void TestWritePing(TestingT *t) {
    test_write_ping(t, false);
}

static void TestWritePingAck(TestingT *t) {
    test_write_ping(t, true);
}

static void TestReadFrameHeader(TestingT *t) {
    static const struct {
        const char *in;
        Http2FrameHeader want;
    } tests[] = {
        {"\x00\x00\x00"
         "\x00"
         "\x00"
         "\x00\x00\x00\x00",
         {true, 0, 0, 0, 0}},
        {"\x01\x02\x03"
         "\x04"
         "\x05"
         "\x06\x07\x08\x09",
         {true, 4, 5, 66051, 101124105}},
        /* Ignore high bit: */
        {"\xff\xff\xff"
         "\xff"
         "\xff"
         "\xff\xff\xff\xff",
         {true, 255, 255, 16777215, 2147483647}},
        {"\xff\xff\xff"
         "\xff"
         "\xff"
         "\x7f\xff\xff\xff",
         {true, 255, 255, 16777215, 2147483647}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        BytesBuffer *r = bytes_new_buffer_string(
            heap_allocator(), str_from_bytes((const Byte *)tests[i].in, 9));
        Error err = BURROW_NO_ERROR;
        Http2FrameHeader got =
            burrow__http2_read_frame_header(bytes_buffer_as_io_reader(r), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d. readFrameHeader(%q) = %v", (int)i,
                               bs_n(tests[i].in, 9), err);
        else if (!header_eq(got, tests[i].want))
            testing_t_errorf_v(t, "%d. readFrameHeader(%q) = %s; want %s", (int)i,
                               bs_n(tests[i].in, 9), header_string(ta(t), got),
                               header_string(ta(t), tests[i].want));
        bytes_buffer_free(r);
    }
}

static void TestReadWriteFrameHeader(TestingT *t) {
    static const struct {
        uint32_t len;
        Http2FrameType typ;
        Http2Flags flags;
        uint32_t stream_id;
    } tests[] = {
        {0, 255, 1, 0},     {0, 255, 1, 1},     {0, 255, 1, 255},
        {0, 255, 1, 256},   {0, 255, 1, 65535}, {0, 255, 1, 65536},

        {0, 1, 255, 1},     {255, 1, 255, 1},   {256, 1, 255, 1},
        {65535, 1, 255, 1}, {65536, 1, 255, 1}, {16777215, 1, 255, 1},
    };
    Byte *zeros = (Byte *)mem_alloc(heap_allocator(), 16777215, 1);
    if (zeros == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        burrow__http2_framer_start_write(fr, tests[i].typ, tests[i].flags,
                                         tests[i].stream_id);
        burrow__http2_framer_write_bytes(fr,
                                         bs_n((const char *)zeros, (Int)tests[i].len));
        (void)burrow__http2_framer_end_write(fr);
        Error err = BURROW_NO_ERROR;
        Http2FrameHeader fh =
            burrow__http2_read_frame_header(bytes_buffer_as_io_reader(&tf.buf), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d. ReadFrameHeader = %v", (int)i, err);
        else if (fh.type != tests[i].typ || fh.flags != tests[i].flags ||
                 fh.length != tests[i].len || fh.stream_id != tests[i].stream_id)
            testing_t_errorf_v(t, "%d. ReadFrameHeader = %s; mismatch", (int)i,
                               header_string(ta(t), fh));
        tf_free(&tf);
    }
    mem_free(heap_allocator(), zeros, 16777215, 1);
}

static void TestWriteTooLargeFrame(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Byte *zeros = (Byte *)mem_alloc(heap_allocator(), (size_t)1 << 24, 1);
    if (zeros == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        tf_free(&tf);
        return;
    }
    burrow__http2_framer_start_write(fr, 0, 1, 1);
    burrow__http2_framer_write_bytes(fr, bs_n((const char *)zeros, (Int)1 << 24));
    Error err = burrow__http2_framer_end_write(fr);
    if (!is_same_error(err, burrow__http2_err_frame_too_large))
        testing_t_errorf_v(t, "endWrite = %v; want errFrameTooLarge", err);
    mem_free(heap_allocator(), zeros, (size_t)1 << 24, 1);
    tf_free(&tf);
}

static void TestWriteGoAway(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Error err =
        burrow__http2_framer_write_go_away(fr, 0x01020304, 0x05060708, bs("foo"));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (!BUF_IS(&tf, "\x00\x00\v\a\x00\x00\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08"
                     "foo"))
        testing_t_errorf_v(t, "encoded as %q", bytes_buffer_bytes(&tf.buf));
    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (f->kind != HTTP2_GO_AWAY_FRAME ||
        !header_eq(f->header, fhdr(0x7, 0, 4 + 4 + 3, 0)) ||
        f->u.go_away.last_stream_id != 0x01020304 ||
        f->u.go_away.err_code != 0x05060708 ||
        !slice_eq(f->u.go_away.debug_data, bs("foo"))) {
        testing_t_fatalf_v(t, "parsed back:\n%s", header_string(ta(t), f->header));
        burrow__http2_frame_free(f);
        tf_free(&tf);
        return;
    }
    Slice got = burrow__http2_go_away_frame_debug_data(f);
    if (!slice_eq(got, bs("foo")))
        testing_t_errorf_v(t, "debug data = %q; want %q", got, BURROW_S("foo"));
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

static void TestWritePushPromise(TestingT *t) {
    Http2PushPromiseParam pp = {0};
    pp.stream_id = 42;
    pp.promise_id = 42;
    pp.block_fragment = bs("abc");
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Error err = burrow__http2_framer_write_push_promise(fr, pp);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (!BUF_IS(&tf, "\x00\x00\x07\x05\x00\x00\x00\x00*\x00\x00\x00*abc"))
        testing_t_errorf_v(t, "encoded as %q", bytes_buffer_bytes(&tf.buf));
    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }
    if (f->kind != HTTP2_PUSH_PROMISE_FRAME) {
        testing_t_fatalf_v(t, "got kind %d; want *PushPromiseFrame", (int)f->kind);
        burrow__http2_frame_free(f);
        tf_free(&tf);
        return;
    }
    if (!header_eq(f->header, fhdr(0x5, 0, 7, 42)) ||
        f->u.push_promise.promise_id != 42 ||
        !slice_eq(f->u.push_promise.header_frag_buf, bs("abc")))
        testing_t_fatalf_v(t, "parsed back:\n%s", header_string(ta(t), f->header));
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

/* A step of TestReadFrameOrder: HEADERS when head, else CONTINUATION. */
typedef struct OrderStep {
    bool head;
    uint32_t id;
    bool end;
} OrderStep;

/* test checkFrameOrder and that HEADERS and CONTINUATION frames can't be
 * intermingled. */
static void TestReadFrameOrder(TestingT *t) {
    static const struct {
        OrderStep w[6];
        int nw;
        int at_least;
        const char *want_err;
    } tests[] = {
        {{{true, 1, true}}, 1, 0, ""},
        {{{true, 1, true}, {true, 2, true}}, 2, 0, ""},
        {{{true, 1, false}, {true, 2, true}},
         2,
         0,
         "got HEADERS for stream 2; expected CONTINUATION following HEADERS for stream "
         "1"},
        {{{true, 1, false}},
         1,
         0,
         "got DATA for stream 1; expected CONTINUATION following HEADERS for stream 1"},
        {{{true, 1, false}, {false, 1, true}, {true, 2, true}}, 3, 0, ""},
        {{{true, 1, false}, {false, 2, true}, {true, 2, true}},
         3,
         0,
         "got CONTINUATION for stream 2; expected stream 1"},
        {{{false, 1, true}}, 1, 0, "unexpected CONTINUATION for stream 1"},
        {{{false, 1, false}}, 1, 0, "unexpected CONTINUATION for stream 1"},
        {{{true, 0, true}}, 1, 0, "HEADERS frame with stream ID 0"},
        {{{false, 0, true}}, 1, 0, "unexpected CONTINUATION for stream 0"},
        {{{true, 1, false},
          {false, 1, false},
          {false, 1, false},
          {false, 1, false},
          {false, 1, true},
          {false, 1, false}},
         6,
         5,
         "unexpected CONTINUATION for stream 1"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TF tf;
        Http2Framer *f = tf_init(&tf);
        f->allow_illegal_writes = true;
        for (int s = 0; s < tests[i].nw; s++) {
            OrderStep st = tests[i].w[s];
            if (st.head) {
                Http2HeadersFrameParam p = {0};
                p.stream_id = st.id;
                p.block_fragment = bs("foo"); /* unused, but non-empty */
                p.end_headers = st.end;
                (void)burrow__http2_framer_write_headers(f, p);
            } else {
                (void)burrow__http2_framer_write_continuation(f, st.id, st.end,
                                                              bs("foo"));
            }
        }
        /* to test transition away from last step */
        (void)burrow__http2_framer_write_data(f, 1, true, (Slice){0});

        Error err = BURROW_NO_ERROR;
        int n = 0;
        BytesBuffer log = BYTES_BUFFER(ta(t));
        for (;;) {
            Http2Frame *got = burrow__http2_framer_read_frame(f, &err);
            Str summary = got != NULL ? burrow__http2_summarize_frame(ta(t), got)
                                      : BURROW_S("<nil>");
            (void)fmt_fprintf_v(bytes_buffer_as_io_writer(&log), "  read %s, %v\n",
                                summary, err);
            burrow__http2_frame_free(got);
            if (BURROW_FAILED(err))
                break;
            n++;
        }
        if (errors_is(err, io_eof))
            err = BURROW_NO_ERROR;
        bool ok = tests[i].want_err[0] == '\0';
        Str logs = bytes_buffer_string(&log, ta(t));
        if (ok && BURROW_FAILED(err)) {
            testing_t_errorf_v(
                t, "%d. after %d good frames, ReadFrame = %v; want success\n%s", (int)i,
                n, err, logs);
            tf_free(&tf);
            continue;
        }
        if (!ok && !is_connection_error(err, HTTP2_ERR_CODE_PROTOCOL)) {
            testing_t_errorf_v(t,
                               "%d. after %d good frames, ReadFrame = %v; want "
                               "ConnectionError(ErrCodeProtocol)\n%s",
                               (int)i, n, err, logs);
            tf_free(&tf);
            continue;
        }
        Error detail = burrow__http2_framer_error_detail(f);
        Str want_err = str_from_cstr(tests[i].want_err);
        if (!((BURROW_OK(detail) && ok) || str_eq(error_text(detail), want_err)))
            testing_t_errorf_v(t, "%d. framer error = %q; want %q\n%s", (int)i,
                               error_text(detail), want_err, logs);
        if (n < tests[i].at_least)
            testing_t_errorf_v(t,
                               "%d. framer only read %d frames; want at least %d\n%s",
                               (int)i, n, tests[i].at_least, logs);
        tf_free(&tf);
    }
}

/* encodeHeaderRaw. */
static Slice encode_header_raw(TestingT *t, BytesBuffer *out, const Str *headers,
                               int n) {
    HpackEncoder *enc =
        burrow__hpack_new_encoder(ta(t), bytes_buffer_as_io_writer(out));
    for (int i = 0; i + 1 < n; i += 2) {
        HpackHeaderField hf = {headers[i], headers[i + 1], false};
        Error err = burrow__hpack_encoder_write_field(enc, hf);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "HPACK encoding error for %q/%q: %v", hf.name,
                               hf.value, err);
    }
    burrow__hpack_encoder_free(enc);
    return bytes_buffer_bytes(out);
}

/* write in TestMetaFrameHeader: the block split at cuts. */
static void write_frags(Http2Framer *f, Slice all, const Int *cuts, int ncuts) {
    Int from = 0;
    for (int i = 0; i <= ncuts; i++) {
        Int to = i < ncuts ? cuts[i] : all.len;
        Slice frag = bs_n((const char *)all.p + from, to - from);
        bool end = i == ncuts;
        if (i == 0) {
            Http2HeadersFrameParam p = {0};
            p.stream_id = 1;
            p.block_fragment = frag;
            p.end_headers = end;
            (void)burrow__http2_framer_write_headers(f, p);
        } else {
            (void)burrow__http2_framer_write_continuation(f, 1, end, frag);
        }
        from = to;
    }
}

enum { META_MAX_PAIRS = 220 };

static void TestMetaFrameHeader(TestingT *t) {
    char one_kb[1025];
    memset(one_kb, 'a', 1024);
    one_kb[1024] = '\0';

    static const Str req[] = {BURROW_S_INIT(":method"), BURROW_S_INIT("GET"),
                              BURROW_S_INIT(":path"), BURROW_S_INIT("/")};
    static const Str req_foo[] = {BURROW_S_INIT(":method"), BURROW_S_INIT("GET"),
                                  BURROW_S_INIT(":path"),   BURROW_S_INIT("/"),
                                  BURROW_S_INIT("foo"),     BURROW_S_INIT("bar")};
    const Str req_big[] = {req[0],          req[1],
                           req[2],          req[3],
                           BURROW_S("foo"), str_from_bytes((const Byte *)one_kb, 1024)};
    Str many[4 + 200];
    memcpy(many, req, sizeof req);
    for (int i = 0; i < 100; i++) {
        many[4 + 2 * i] = BURROW_S("foo");
        many[4 + 2 * i + 1] = BURROW_S("bar");
    }
    static const Str pseudo_order[] = {BURROW_S_INIT(":method"), BURROW_S_INIT("GET"),
                                       BURROW_S_INIT("foo"),     BURROW_S_INIT("bar"),
                                       BURROW_S_INIT(":path"),   BURROW_S_INIT("/")};
    static const Str pseudo_unknown[] = {BURROW_S_INIT(":unknown"),
                                         BURROW_S_INIT("foo"), BURROW_S_INIT("foo"),
                                         BURROW_S_INIT("bar")};
    static const Str pseudo_mix[] = {BURROW_S_INIT(":method"), BURROW_S_INIT("GET"),
                                     BURROW_S_INIT(":status"), BURROW_S_INIT("100")};
    static const Str pseudo_dup[] = {BURROW_S_INIT(":method"), BURROW_S_INIT("GET"),
                                     BURROW_S_INIT(":method"), BURROW_S_INIT("POST")};
    static const Str trailer[] = {BURROW_S_INIT("foo"), BURROW_S_INIT("bar")};
    static const Str bad_name[] = {BURROW_S_INIT("CapitalBad"), BURROW_S_INIT("x")};
    static const Str bad_value[] = {BURROW_S_INIT("key"),
                                    BURROW_S_INIT("bad_null\x00")};
    static const Int cut1[] = {1};
    static const Int cut2[] = {2};
    static const Int cut24[] = {2, 4};

    struct {
        const char *name;
        const Str *headers;
        const Int *cuts;
        const char *want_err_reason;
        int nheaders;
        int ncuts;
        /* The MetaHeadersFrame wanted, when want_err is 0. */
        uint32_t length;
        int npairs;
        /* 1 for ConnectionError(ErrCodeCompression), 2 for streamError(1,
         * ErrCodeProtocol). */
        int want_err;
        uint32_t max_header_list_size;
        Http2Flags flags;
        bool truncated;
    } tests[13];
    memset(tests, 0, sizeof tests);

    tests[0].name = "single_headers";
    tests[0].headers = req;
    tests[0].nheaders = 4;
    tests[0].flags = HTTP2_FLAG_HEADERS_END_HEADERS;
    tests[0].length = 2;
    tests[0].npairs = 4;

    tests[1].name = "with_continuation";
    tests[1].headers = req_foo;
    tests[1].nheaders = 6;
    tests[1].cuts = cut1;
    tests[1].ncuts = 1;
    tests[1].length = 1;
    tests[1].npairs = 6;

    tests[2].name = "with_two_continuation";
    tests[2].headers = req_foo;
    tests[2].nheaders = 6;
    tests[2].cuts = cut24;
    tests[2].ncuts = 2;
    tests[2].length = 2;
    tests[2].npairs = 6;

    tests[3].name = "big_string_okay";
    tests[3].headers = req_big;
    tests[3].nheaders = 6;
    tests[3].cuts = cut2;
    tests[3].ncuts = 1;
    tests[3].length = 2;
    tests[3].npairs = 6;

    tests[4].name = "big_string_error";
    tests[4].headers = req_big;
    tests[4].nheaders = 6;
    tests[4].cuts = cut2;
    tests[4].ncuts = 1;
    tests[4].max_header_list_size = (1 << 10) / 2;
    tests[4].want_err = 1;

    tests[5].name = "max_header_list_truncated";
    tests[5].headers = many;
    tests[5].nheaders = 204;
    tests[5].cuts = cut2;
    tests[5].ncuts = 1;
    tests[5].max_header_list_size = (1 << 10) / 2;
    tests[5].length = 2;
    tests[5].npairs = 4 + 2 * 11; /* 11 foo: bar */
    tests[5].truncated = true;

    tests[6].name = "pseudo_order";
    tests[6].headers = pseudo_order;
    tests[6].nheaders = 6;
    tests[6].want_err = 2;
    tests[6].want_err_reason = "pseudo header field after regular";

    tests[7].name = "pseudo_unknown";
    tests[7].headers = pseudo_unknown;
    tests[7].nheaders = 4;
    tests[7].want_err = 2;
    tests[7].want_err_reason = "invalid pseudo-header \":unknown\"";

    tests[8].name = "pseudo_mix_request_response";
    tests[8].headers = pseudo_mix;
    tests[8].nheaders = 4;
    tests[8].want_err = 2;
    tests[8].want_err_reason = "mix of request and response pseudo headers";

    tests[9].name = "pseudo_dup";
    tests[9].headers = pseudo_dup;
    tests[9].nheaders = 4;
    tests[9].want_err = 2;
    tests[9].want_err_reason = "duplicate pseudo-header \":method\"";

    tests[10].name = "trailer_okay_no_pseudo";
    tests[10].headers = trailer;
    tests[10].nheaders = 2;
    tests[10].flags = HTTP2_FLAG_HEADERS_END_HEADERS;
    tests[10].length = 8;
    tests[10].npairs = 2;

    tests[11].name = "invalid_field_name";
    tests[11].headers = bad_name;
    tests[11].nheaders = 2;
    tests[11].want_err = 2;
    tests[11].want_err_reason = "invalid header field name \"CapitalBad\"";

    tests[12].name = "invalid_field_value";
    tests[12].headers = bad_value;
    tests[12].nheaders = 2;
    tests[12].want_err = 2;
    tests[12].want_err_reason = "invalid header field value for \"key\"";

    for (int i = 0; i < 13; i++) {
        Str name = str_from_cstr(tests[i].name);
        TF tf;
        Http2Framer *f = tf_init(&tf);
        HpackDecoder *dec = burrow__hpack_new_decoder(
            heap_allocator(), HTTP2_INITIAL_HEADER_TABLE_SIZE, NULL, NULL);
        f->read_meta_headers = dec;
        f->max_header_list_size = tests[i].max_header_list_size;
        BytesBuffer block = BYTES_BUFFER(ta(t));
        Slice all = encode_header_raw(t, &block, tests[i].headers, tests[i].nheaders);
        write_frags(f, all, tests[i].cuts, tests[i].ncuts);

        Error err = BURROW_NO_ERROR;
        Http2Frame *got = burrow__http2_framer_read_frame(f, &err);
        Str want_reason = tests[i].want_err_reason != NULL
                              ? str_from_cstr(tests[i].want_err_reason)
                              : BURROW_STR_EMPTY;
        if (tests[i].want_err == 1) {
            if (!is_connection_error(err, HTTP2_ERR_CODE_COMPRESSION))
                testing_t_errorf_v(
                    t, "%s:\n got: %v\nwant: error %v", name, err,
                    burrow__http2_connection_error(HTTP2_ERR_CODE_COMPRESSION));
        } else if (tests[i].want_err == 2) {
            /* Ignore the StreamError.Cause field, if it matches the
             * wantErrReason. The test table above predates the Cause field. */
            Http2StreamError se;
            if (!burrow__http2_error_stream(err, &se) || se.stream_id != 1 ||
                se.code != HTTP2_ERR_CODE_PROTOCOL ||
                (BURROW_FAILED(se.cause) && !str_eq(error_text(se.cause), want_reason)))
                testing_t_errorf_v(
                    t,
                    "%s:\n got: %v\nwant: error stream error: stream ID 1; "
                    "PROTOCOL_ERROR",
                    name, err);
            if (got != NULL)
                testing_t_errorf_v(t, "%s: got a frame with the error", name);
        } else if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s:\n got: error %v", name, err);
        } else {
            Http2FrameHeader want_header =
                fhdr(HTTP2_FRAME_HEADERS, tests[i].flags, tests[i].length, 1);
            want_header.valid = false;
            const HpackHeaderFields *fs = &got->u.meta_headers.fields;
            bool same = got->kind == HTTP2_META_HEADERS_FRAME &&
                        header_eq(got->header, want_header) &&
                        burrow__http2_priority_param_is_zero(
                            got->u.meta_headers.headers.priority) &&
                        got->u.meta_headers.headers.header_frag_buf.len == 0 &&
                        got->u.meta_headers.truncated == tests[i].truncated &&
                        fs->len * 2 == tests[i].npairs;
            for (Int k = 0; same && k < fs->len; k++)
                same = str_eq(fs->p[k].name, tests[i].headers[2 * k]) &&
                       str_eq(fs->p[k].value, tests[i].headers[2 * k + 1]) &&
                       !fs->p[k].sensitive;
            if (!same)
                testing_t_errorf_v(t, "%s:\n got: %s, %d fields, truncated %t", name,
                                   header_string(ta(t), got->header), fs->len,
                                   got->u.meta_headers.truncated);
        }
        Error detail = burrow__http2_framer_error_detail(f);
        if (want_reason.len > 0 && !str_eq(want_reason, error_text(detail)))
            testing_t_errorf_v(t, "%s: got error reason %q; want %q", name,
                               error_text(detail), want_reason);
        burrow__http2_frame_free(got);
        bytes_buffer_free(&block);
        burrow__hpack_decoder_free(dec);
        tf_free(&tf);
    }
}

static Http2Frame *read_and_verify_data_frame(const char *data, Byte length, TF *tf,
                                              TestingT *t) {
    uint32_t stream_id = (1U << 24) + (2U << 16) + (3U << 8) + 4U;
    Int n = (Int)strlen(data);
    (void)burrow__http2_framer_write_data(tf->fr, stream_id, true, bs(data));
    Byte want_enc[9 + 3];
    Byte head[9] = {0, 0, length, 0, 1, 1, 2, 3, 4};
    memcpy(want_enc, head, 9);
    memcpy(want_enc + 9, data, (size_t)n);
    if (!slice_is(bytes_buffer_bytes(&tf->buf), (const char *)want_enc, 9 + n))
        testing_t_errorf_v(t, "encoded as %q; want %q", bytes_buffer_bytes(&tf->buf),
                           bs_n((const char *)want_enc, 9 + n));
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = burrow__http2_framer_read_frame(tf->fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        return NULL;
    }
    if (f->kind != HTTP2_DATA_FRAME) {
        testing_t_fatalf_v(t, "got kind %d; want *DataFrame", (int)f->kind);
        burrow__http2_frame_free(f);
        return NULL;
    }
    Slice got = burrow__http2_data_frame_data(f);
    if (!slice_eq(got, bs(data)))
        testing_t_errorf_v(t, "got %q; want %q", got, bs(data));
    if ((f->header.flags & 1) == 0)
        testing_t_errorf_v(t, "didn't see END_STREAM flag");
    return f;
}

static void TestSetReuseFrames(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    burrow__http2_framer_set_reuse_frames(fr);

    /* Check that DataFrames are reused. Note that SetReuseFrames only
     * currently implements reuse of DataFrames. */
    Http2Frame *first_df = read_and_verify_data_frame("ABC", 3, &tf, t);
    if (first_df == NULL) {
        tf_free(&tf);
        return;
    }
    static const struct {
        const char *data;
        Byte length;
    } rounds[] = {{"XYZ", 3}, {"", 0}, {"HHH", 3}};
    for (size_t r = 0; r < sizeof rounds / sizeof rounds[0]; r++) {
        for (int i = 0; i < 10; i++) {
            Http2Frame *df =
                read_and_verify_data_frame(rounds[r].data, rounds[r].length, &tf, t);
            if (df == NULL) {
                tf_free(&tf);
                return;
            }
            if (df != first_df)
                testing_t_errorf_v(t,
                                   "Expected Framer to return references to the same "
                                   "DataFrame. Have %p and %p",
                                   (void *)df, (void *)first_df);
            burrow__http2_frame_free(df);
        }
    }
    tf_free(&tf);
}

static void TestSetReuseFramesMoreThanOnce(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    burrow__http2_framer_set_reuse_frames(fr);

    Http2Frame *first_df = read_and_verify_data_frame("ABC", 3, &tf, t);
    if (first_df == NULL) {
        tf_free(&tf);
        return;
    }
    burrow__http2_framer_set_reuse_frames(fr);

    for (int i = 0; i < 10; i++) {
        Http2Frame *df = read_and_verify_data_frame("XYZ", 3, &tf, t);
        if (df == NULL)
            break;
        /* SetReuseFrames should be idempotent */
        burrow__http2_framer_set_reuse_frames(fr);
        if (df != first_df)
            testing_t_errorf_v(
                t,
                "Expected Framer to return references to the same DataFrame. "
                "Have %p and %p",
                (void *)df, (void *)first_df);
    }
    tf_free(&tf);
}

static void TestNoSetReuseFrames(TestingT *t) {
    enum { NUM_NEW_DATA_FRAMES = 10 };
    TF tf;
    (void)tf_init(&tf);
    Http2Frame *df_so_far[NUM_NEW_DATA_FRAMES] = {0};

    /* Check that DataFrames are not reused if SetReuseFrames wasn't called.
     * SetReuseFrames only currently implements reuse of DataFrames. */
    for (int i = 0; i < NUM_NEW_DATA_FRAMES; i++) {
        Http2Frame *df = read_and_verify_data_frame("XYZ", 3, &tf, t);
        if (df == NULL)
            break;
        for (int j = 0; j < i; j++) {
            if (df == df_so_far[j])
                testing_t_errorf_v(t, "Expected Framer to return new DataFrames since "
                                      "SetNoReuseFrames not set.");
        }
        df_so_far[i] = df;
    }
    for (int i = 0; i < NUM_NEW_DATA_FRAMES; i++)
        burrow__http2_frame_free(df_so_far[i]);
    tf_free(&tf);
}

static void TestSettingsDuplicates(TestingT *t) {
    static const struct {
        Http2SettingID ids[12];
        int n;
        bool want;
    } tests[] = {
        {{0}, 0, false},
        {{1}, 1, false},
        {{1, 2}, 2, false},
        {{1, 2}, 2, false},
        {{1, 2, 3}, 3, false},
        {{1, 2, 3}, 3, false},
        {{1, 2, 3, 4}, 4, false},

        {{1, 2, 3, 2}, 4, true},
        {{4, 2, 3, 4}, 4, true},

        {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, 12, false},

        {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 11}, 12, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        Http2Setting settings[12];
        memset(settings, 0, sizeof settings);
        for (int k = 0; k < tests[i].n; k++)
            settings[k].id = tests[i].ids[k];
        (void)burrow__http2_framer_write_settings(fr, settings, tests[i].n);
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%d. ReadFrame: %v", (int)i, err);
            tf_free(&tf);
            return;
        }
        bool got = burrow__http2_settings_frame_has_duplicates(f);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%d. HasDuplicates = %t; want %t", (int)i, got,
                               tests[i].want);
        burrow__http2_frame_free(f);
        tf_free(&tf);
    }
}

static void TestTypeFrameParser(TestingT *t) {
    /* Go checks len(frameNames) == len(frameParsers). Here the last name and
     * the first type past it stand in for that. */
    Byte buf[HTTP2_STRING_MAX];
    CHECK(str_eq(burrow__http2_frame_type_string(HTTP2_FRAME_PRIORITY_UPDATE, buf),
                 BURROW_S("PRIORITY_UPDATE")));
    CHECK(str_eq(burrow__http2_frame_type_string(HTTP2_FRAME_PRIORITY_UPDATE + 1, buf),
                 BURROW_S("UNKNOWN_FRAME_TYPE_17")));

    /* typeFrameParser() for an unknown type returns a function that returns
     * UnknownFrame */
    Http2FrameType unknown_frame_type = HTTP2_FRAME_PRIORITY_UPDATE + 1;
    Http2FrameParser unknown_parser =
        burrow__http2_type_frame_parser(unknown_frame_type);
    Http2Frame frame = {0};
    Http2FrameHeader fh = {0};
    Error err = unknown_parser(&frame, fh, (Http2CountErrorFunc){0}, (Slice){0});
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unknownParser() must not return an error: %v", err);
    if (frame.kind != HTTP2_UNKNOWN_FRAME)
        testing_t_errorf_v(t, "expected UnknownFrame, got kind %d", (int)frame.kind);
}

static void TestReadFrameHeaderAndBody(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    uint32_t stream_id = 1;
    Slice data = bs("ABC");
    Error err = burrow__http2_framer_write_data(fr, stream_id, true, data);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "WriteData(%d, true, %q) failed: %v", stream_id, data,
                           err);
        tf_free(&tf);
        return;
    }

    Http2FrameHeader fh = burrow__http2_framer_read_frame_header(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ReadFrameHeader failed: %v", err);
        tf_free(&tf);
        return;
    }
    Http2FrameHeader want_header =
        fhdr(HTTP2_FRAME_DATA, HTTP2_FLAG_DATA_END_STREAM, 3, 1);
    if (!header_eq(fh, want_header)) {
        testing_t_fatalf_v(t, "ReadFrameHeader = %s; want %s", header_string(ta(t), fh),
                           header_string(ta(t), want_header));
        tf_free(&tf);
        return;
    }

    Http2Frame *f = burrow__http2_framer_read_frame_for_header(fr, fh, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ReadFrameForHeader failed: %v", err);
        tf_free(&tf);
        return;
    }

    if (!header_eq(fh, f->header)) {
        testing_t_fatalf_v(t, "Frame.Header() = %s; want %s",
                           header_string(ta(t), f->header), header_string(ta(t), fh));
        burrow__http2_frame_free(f);
        tf_free(&tf);
        return;
    }

    if (f->kind != HTTP2_DATA_FRAME) {
        testing_t_fatalf_v(t, "got kind %d; want *DataFrame", (int)f->kind);
        burrow__http2_frame_free(f);
        tf_free(&tf);
        return;
    }
    Slice got = burrow__http2_data_frame_data(f);
    if (!slice_eq(got, data))
        testing_t_errorf_v(t, "DataFrame.Data() = %q; want %q", got, data);
    if (!burrow__http2_data_frame_stream_ended(f))
        testing_t_errorf_v(t, "DataFrame.StreamEnded() = %t; want %t", false, true);
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

static void TestReadFrameHeaderFrameTooLarge(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    burrow__http2_framer_set_max_read_frame_size(fr, 2);
    Error err = burrow__http2_framer_write_data(fr, 1, true, bs("ABC"));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "WriteData failed: %v", err);
        tf_free(&tf);
        return;
    }
    Http2FrameHeader fh = burrow__http2_framer_read_frame_header(fr, &err);
    if (!is_same_error(err, burrow__http2_err_frame_too_large)) {
        testing_t_fatalf_v(t, "ReadFrameHeader returned error %v; want %v", err,
                           burrow__http2_err_frame_too_large);
        tf_free(&tf);
        return;
    }
    if (fh.stream_id != 1)
        testing_t_errorf_v(t, "ReadFrameHeader = %s, %v; want StreamID 1",
                           header_string(ta(t), fh), err);
    tf_free(&tf);
}

static void TestReadFrameHeaderBadFrameOrder(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Http2HeadersFrameParam p = {0};
    p.stream_id = 1;
    p.block_fragment = bs("foo"); /* unused, but non-empty */
    p.end_headers = false;
    Error err = burrow__http2_framer_write_headers(fr, p);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "WriteHeaders failed: %v", err);
        tf_free(&tf);
        return;
    }

    /* Write a CONTINUATION frame for stream 2 without first finishing the
     * headers for stream 1. */
    err = burrow__http2_framer_write_continuation(fr, 2, true, bs("foo"));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "WriteContinuation failed: %v", err);
        tf_free(&tf);
        return;
    }

    Http2FrameHeader fh = burrow__http2_framer_read_frame_header(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ReadFrameHeader failed: %v", err);
        tf_free(&tf);
        return;
    }
    Http2Frame *f = burrow__http2_framer_read_frame_for_header(fr, fh, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ReadFrameForHeader failed: %v", err);
        tf_free(&tf);
        return;
    }
    burrow__http2_frame_free(f);

    (void)burrow__http2_framer_read_frame_header(fr, &err);
    if (!is_connection_error(err, HTTP2_ERR_CODE_PROTOCOL))
        testing_t_fatalf_v(t,
                           "ReadFrameHeader returned error %v; want "
                           "ConnectionError(ErrCodeProtocol)",
                           err);
    tf_free(&tf);
}

static void TestReadFrameForHeaderUnexpectedEOF(TestingT *t) {
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Error err = burrow__http2_framer_write_data(fr, 1, true, bs("ABC"));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "WriteData failed: %v", err);
        tf_free(&tf);
        return;
    }

    Http2FrameHeader fh = burrow__http2_framer_read_frame_header(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ReadFrameHeader failed: %v", err);
        tf_free(&tf);
        return;
    }

    /* Remove one byte from the body, corrupting the frame body. */
    bytes_buffer_truncate(&tf.buf, bytes_buffer_len(&tf.buf) - 1);

    Http2Frame *f = burrow__http2_framer_read_frame_for_header(fr, fh, &err);
    burrow__http2_frame_free(f);
    if (!is_same_error(err, io_err_unexpected_eof))
        testing_t_fatalf_v(t,
                           "ReadFrameForHeader with short body = %v; want "
                           "io.ErrUnexpectedEOF",
                           err);
    tf_free(&tf);
}

static void TestTypeFrameParserHolePanic(TestingT *t) {
    /* Verify that unassigned frame types (0x0a-0x0f) don't panic.
     * golang.org/issue/77652 */
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    Error err = burrow__http2_framer_write_raw_frame(fr, 0x0a, 0, 1, (Slice){0});
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }

    Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        tf_free(&tf);
        return;
    }

    if (f->kind != HTTP2_UNKNOWN_FRAME)
        testing_t_errorf_v(t, "got kind %d; want *UnknownFrame", (int)f->kind);
    burrow__http2_frame_free(f);
    tf_free(&tf);
}

/* ------------------------------------------------------ burrow's own tests
 *
 * Go has no tests for these, so the expected strings are what Go 1.27.1's
 * http2 package prints for the same input. */

static void TestErrorStrings(TestingT *t) {
    struct {
        Error err;
        const char *want;
    } tests[] = {
        {burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL),
         "connection error: PROTOCOL_ERROR"},
        {burrow__http2_connection_error(0x20),
         "connection error: unknown error code 0x20"},
        {burrow__http2_stream_error(1, HTTP2_ERR_CODE_PROTOCOL, BURROW_NO_ERROR),
         "stream error: stream ID 1; PROTOCOL_ERROR"},
        {burrow__http2_stream_error(7, HTTP2_ERR_CODE_CANCEL,
                                    burrow__http2_err_from_peer),
         "stream error: stream ID 7; CANCEL; received from peer"},
        {burrow__http2_conn_error(
             HTTP2_ERR_CODE_FRAME_SIZE,
             BURROW_S("PRIORITY frame payload size was 4; want 5")),
         "http2: connection error: FRAME_SIZE_ERROR: PRIORITY frame payload size was "
         "4; "
         "want 5"},
        {burrow__http2_pseudo_header_error(BURROW_S(":x\"y")),
         "invalid pseudo-header \":x\\\"y\""},
        {burrow__http2_duplicate_pseudo_header_error(BURROW_S(":method")),
         "duplicate pseudo-header \":method\""},
        {burrow__http2_header_field_name_error(BURROW_S("Bad\x01")),
         "invalid header field name \"Bad\\x01\""},
        {burrow__http2_header_field_value_error(BURROW_S("key")),
         "invalid header field value for \"key\""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = error_text(tests[i].err);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%d. Error() = %q; want %q", (int)i, got,
                               str_from_cstr(tests[i].want));
    }
    Byte buf[HTTP2_STRING_MAX];
    CHECK(str_eq(burrow__http2_err_code_string_token(15, buf),
                 BURROW_S("ERR_UNKNOWN_15")));
}

static void TestSettingString(TestingT *t) {
    Http2Setting max_frame = {HTTP2_SETTING_MAX_FRAME_SIZE, 16384};
    Http2Setting unknown = {7, 1};
    CHECK(str_eq(burrow__http2_setting_string(ta(t), max_frame),
                 BURROW_S("[MAX_FRAME_SIZE = 16384]")));
    CHECK(str_eq(burrow__http2_setting_string(ta(t), unknown),
                 BURROW_S("[UNKNOWN_SETTING_7 = 1]")));
}

static void TestFrameHeaderString(TestingT *t) {
    struct {
        Http2FrameHeader h;
        const char *want;
    } tests[] = {
        {fhdr(HTTP2_FRAME_DATA, HTTP2_FLAG_DATA_END_STREAM | HTTP2_FLAG_DATA_PADDED, 3,
              1),
         "[FrameHeader DATA flags=END_STREAM|PADDED stream=1 len=3]"},
        {fhdr(HTTP2_FRAME_HEADERS, 0xff, 0, 0),
         "[FrameHeader HEADERS "
         "flags=END_STREAM|0x2|END_HEADERS|PADDED|0x10|PRIORITY|0x40|0x80 "
         "len=0]"},
        /* 0x0b is one of the unassigned types, which Go names "". */
        {fhdr(0x0b, 0x2, 1, 9), "[FrameHeader  flags=0x2 stream=9 len=1]"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = header_string(ta(t), tests[i].h);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%d. String() = %q; want %q", (int)i, got,
                               str_from_cstr(tests[i].want));
    }
}

/* Writes the frame for case i of TestSummarizeFrame. */
static void summary_write(Http2Framer *f, int i) {
    static const Byte ping[8] = {1, 2, 3, 4, 5, 6, 7, 'z'};
    if (i == 0) {
        (void)burrow__http2_framer_write_data(f, 1, true, bs_n("ABC\x00\"", 5));
    } else if (i == 1) {
        Byte data[300];
        memset(data, 'x', sizeof data);
        (void)burrow__http2_framer_write_data(f, 3, false,
                                              bs_n((const char *)data, 300));
    } else if (i == 2) {
        Http2Setting s[] = {{1, 2}, {7, 4}};
        (void)burrow__http2_framer_write_settings(f, s, 2);
    } else if (i == 3) {
        (void)burrow__http2_framer_write_settings(f, NULL, 0);
    } else if (i == 4) {
        (void)burrow__http2_framer_write_settings_ack(f);
    } else if (i == 5) {
        (void)burrow__http2_framer_write_window_update(f, 0, 1000);
    } else if (i == 6) {
        (void)burrow__http2_framer_write_window_update(f, 5, 1);
    } else if (i == 7) {
        (void)burrow__http2_framer_write_ping(f, true, ping);
    } else if (i == 8) {
        (void)burrow__http2_framer_write_go_away(f, 9, HTTP2_ERR_CODE_ENHANCE_YOUR_CALM,
                                                 bs("bye"));
    } else if (i == 9) {
        (void)burrow__http2_framer_write_rst_stream(f, 3, 0x99);
    } else if (i == 10) {
        Http2HeadersFrameParam p = {0};
        p.stream_id = 1;
        p.block_fragment = bs("ab");
        p.end_headers = true;
        p.pad_length = 1;
        p.priority = prio(3, false, 9);
        (void)burrow__http2_framer_write_headers(f, p);
    } else if (i == 11) {
        (void)burrow__http2_framer_write_raw_frame(f, 0x20, 0x81, 4, bs("q"));
    } else {
        (void)burrow__http2_framer_write_priority_update(f, 5, BURROW_S("u=1"));
    }
}

static void TestSummarizeFrame(TestingT *t) {
    static const char *const tests[] = {
        "DATA flags=END_STREAM stream=1 len=5 data=\"ABC\\x00\\\"\"",
        NULL, /* big_want */
        "SETTINGS len=12, settings: HEADER_TABLE_SIZE=2, UNKNOWN_SETTING_7=4",
        "SETTINGS len=0",
        "SETTINGS flags=ACK len=0",
        "WINDOW_UPDATE len=4 (conn) incr=1000",
        "WINDOW_UPDATE stream=5 len=4 incr=1",
        "PING flags=ACK len=8 ping=\"\\x01\\x02\\x03\\x04\\x05\\x06\\az\"",
        "GOAWAY len=11 LastStreamID=9 ErrCode=ENHANCE_YOUR_CALM Debug=\"bye\"",
        "RST_STREAM stream=3 len=4 ErrCode=unknown error code 0x99",
        "HEADERS flags=END_HEADERS|PADDED|PRIORITY stream=1 len=9",
        "UNKNOWN_FRAME_TYPE_32 flags=0x1|0x80 stream=4 len=1",
        "PRIORITY_UPDATE len=7",
    };
    /* A DATA summary shows the first 256 bytes. */
    static const char big_head[] = "DATA stream=3 len=300 data=\"";
    static const char big_tail[] = "\" (44 bytes omitted)";
    char big_want[sizeof big_head - 1 + 256 + sizeof big_tail];
    memcpy(big_want, big_head, sizeof big_head - 1);
    memset(big_want + sizeof big_head - 1, 'x', 256);
    memcpy(big_want + sizeof big_head - 1 + 256, big_tail, sizeof big_tail);

    for (int i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++) {
        Str want = str_from_cstr(i == 1 ? big_want : tests[i]);
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        fr->allow_illegal_reads = true;
        summary_write(fr, i);
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. ReadFrame: %v", i, err);
            tf_free(&tf);
            continue;
        }
        Str got = burrow__http2_summarize_frame(ta(t), f);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%d. summarizeFrame = %q; want %q", i, got, want);
        burrow__http2_frame_free(f);
        tf_free(&tf);
    }
}

static void count_into(void *env, Str token) {
    BytesBuffer *b = (BytesBuffer *)env;
    if (bytes_buffer_len(b) > 0)
        (void)bytes_buffer_write_string(b, BURROW_S(","), NULL);
    (void)bytes_buffer_write_string(b, token, NULL);
}

#define P(s) (s), (Int)sizeof(s) - 1

/* Each malformed frame gives "error|ErrorDetail|countError tokens", with
 * "<nil>" for no error. */
static void TestMalformedFrames(TestingT *t) {
    static const struct {
        const char *name;
        const char *payload;
        Int n;
        const char *want;
        uint32_t stream_id;
        Http2FrameType type;
        Http2Flags flags;
    } tests[] = {
        {"bdata0", P("x"),
         "connection error: PROTOCOL_ERROR|DATA frame with stream ID "
         "0|frame_data_stream_0",
         0, HTTP2_FRAME_DATA, 0},
        {"bdatapad", P(""), "unexpected EOF|<nil>|frame_data_pad_byte_short", 1,
         HTTP2_FRAME_DATA, HTTP2_FLAG_DATA_PADDED},
        {"bdatapadbig",
         P("\x05"
           "ab"),
         "connection error: PROTOCOL_ERROR|pad size larger than data "
         "payload|frame_data_pad_too_big",
         1, HTTP2_FRAME_DATA, HTTP2_FLAG_DATA_PADDED},
        {"bsetack", P("\x00\x01\x00\x00\x00\x00"),
         "connection error: FRAME_SIZE_ERROR|<nil>|frame_settings_ack_with_length", 0,
         HTTP2_FRAME_SETTINGS, HTTP2_FLAG_SETTINGS_ACK},
        {"bsetstream", P(""),
         "connection error: PROTOCOL_ERROR|<nil>|frame_settings_has_stream", 1,
         HTTP2_FRAME_SETTINGS, 0},
        {"bsetmod", P("\x00\x01\x00"),
         "connection error: FRAME_SIZE_ERROR|<nil>|frame_settings_mod_6", 0,
         HTTP2_FRAME_SETTINGS, 0},
        {"bsetwin", P("\x00\x04\x80\x00\x00\x00"),
         "connection error: "
         "FLOW_CONTROL_ERROR|<nil>|frame_settings_window_size_too_big",
         0, HTTP2_FRAME_SETTINGS, 0},
        {"bping", P("1234567"),
         "connection error: FRAME_SIZE_ERROR|<nil>|frame_ping_length", 0,
         HTTP2_FRAME_PING, 0},
        {"bpingstream", P("12345678"),
         "connection error: PROTOCOL_ERROR|<nil>|frame_ping_has_stream", 1,
         HTTP2_FRAME_PING, 0},
        {"bgoawaystream", P("12345678"),
         "connection error: PROTOCOL_ERROR|<nil>|frame_goaway_has_stream", 1,
         HTTP2_FRAME_GO_AWAY, 0},
        {"bgoawayshort", P("1234567"),
         "connection error: FRAME_SIZE_ERROR|<nil>|frame_goaway_short", 0,
         HTTP2_FRAME_GO_AWAY, 0},
        {"bwulen", P("123"),
         "connection error: FRAME_SIZE_ERROR|<nil>|frame_windowupdate_bad_len", 1,
         HTTP2_FRAME_WINDOW_UPDATE, 0},
        {"bwuconn", P("\x80\x00\x00\x00"),
         "connection error: PROTOCOL_ERROR|<nil>|frame_windowupdate_zero_inc_conn", 0,
         HTTP2_FRAME_WINDOW_UPDATE, 0},
        {"bwustream", P("\x00\x00\x00\x00"),
         "stream error: stream ID 3; "
         "PROTOCOL_ERROR|<nil>|frame_windowupdate_zero_inc_stream",
         3, HTTP2_FRAME_WINDOW_UPDATE, 0},
        {"bhdr0", P("x"),
         "connection error: PROTOCOL_ERROR|HEADERS frame with stream ID "
         "0|frame_headers_zero_stream",
         0, HTTP2_FRAME_HEADERS, HTTP2_FLAG_HEADERS_END_HEADERS},
        {"bhdrpad", P(""), "unexpected EOF|<nil>|frame_headers_pad_short", 1,
         HTTP2_FRAME_HEADERS,
         HTTP2_FLAG_HEADERS_END_HEADERS | HTTP2_FLAG_HEADERS_PADDED},
        {"bhdrprio", P("123"), "unexpected EOF|<nil>|frame_headers_prio_short", 1,
         HTTP2_FRAME_HEADERS,
         HTTP2_FLAG_HEADERS_END_HEADERS | HTTP2_FLAG_HEADERS_PRIORITY},
        {"bhdrweight", P("1234"),
         "unexpected EOF|<nil>|frame_headers_prio_weight_short", 1, HTTP2_FRAME_HEADERS,
         HTTP2_FLAG_HEADERS_END_HEADERS | HTTP2_FLAG_HEADERS_PRIORITY},
        {"bhdrpadbig",
         P("\x09"
           "ab"),
         "stream error: stream ID 1; PROTOCOL_ERROR|<nil>|frame_headers_pad_too_big", 1,
         HTTP2_FRAME_HEADERS,
         HTTP2_FLAG_HEADERS_END_HEADERS | HTTP2_FLAG_HEADERS_PADDED},
        {"bprio0", P("12345"),
         "connection error: PROTOCOL_ERROR|PRIORITY frame with stream ID "
         "0|frame_priority_zero_stream",
         0, HTTP2_FRAME_PRIORITY, 0},
        {"bpriolen", P("1234"),
         "connection error: FRAME_SIZE_ERROR|PRIORITY frame payload size was 4; want "
         "5|frame_priority_bad_length",
         1, HTTP2_FRAME_PRIORITY, 0},
        {"bpu", P("\x00\x00\x00\x01"),
         "connection error: PROTOCOL_ERROR|PRIORITY_UPDATE frame with non-zero stream "
         "ID|frame_priority_update_non_zero_stream",
         1, HTTP2_FRAME_PRIORITY_UPDATE, 0},
        {"bpulen", P("\x00\x01"),
         "connection error: FRAME_SIZE_ERROR|PRIORITY_UPDATE frame payload size was 2; "
         "want at "
         "least 4|frame_priority_update_bad_length",
         0, HTTP2_FRAME_PRIORITY_UPDATE, 0},
        {"bpuzero", P("\x80\x00\x00\x00u=1"),
         "connection error: PROTOCOL_ERROR|PRIORITY_UPDATE frame with prioritized "
         "stream ID of "
         "zero|frame_priority_update_prioritizing_zero_stream",
         0, HTTP2_FRAME_PRIORITY_UPDATE, 0},
        {"brstlen", P("12"),
         "connection error: FRAME_SIZE_ERROR|<nil>|frame_rststream_bad_len", 1,
         HTTP2_FRAME_RST_STREAM, 0},
        {"brst0", P("1234"),
         "connection error: PROTOCOL_ERROR|<nil>|frame_rststream_zero_stream", 0,
         HTTP2_FRAME_RST_STREAM, 0},
        /* The order check catches this one before a parser can count it. */
        {"bcont0", P("x"),
         "connection error: PROTOCOL_ERROR|unexpected CONTINUATION for stream 0|", 0,
         HTTP2_FRAME_CONTINUATION, 0},
        {"bpp0", P("1234"),
         "connection error: PROTOCOL_ERROR|<nil>|frame_pushpromise_zero_stream", 0,
         HTTP2_FRAME_PUSH_PROMISE, 0},
        {"bpppad", P(""), "unexpected EOF|<nil>|frame_pushpromise_pad_short", 1,
         HTTP2_FRAME_PUSH_PROMISE, HTTP2_FLAG_PUSH_PROMISE_PADDED},
        {"bppid", P("123"), "unexpected EOF|<nil>|frame_pushpromise_promiseid_short", 1,
         HTTP2_FRAME_PUSH_PROMISE, 0},
        {"bpppadbig",
         P("\x02\x00\x00\x00\x02"
           "a"),
         "connection error: PROTOCOL_ERROR|<nil>|frame_pushpromise_pad_too_big", 1,
         HTTP2_FRAME_PUSH_PROMISE, HTTP2_FLAG_PUSH_PROMISE_PADDED},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        Alloc *a = ta(t);
        BytesBuffer tokens = BYTES_BUFFER(a);
        fr->count_error = BURROW_FN(Http2CountErrorFunc, count_into, &tokens);
        (void)burrow__http2_framer_write_raw_frame(fr, tests[i].type, tests[i].flags,
                                                   tests[i].stream_id,
                                                   bs_n(tests[i].payload, tests[i].n));
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        burrow__http2_frame_free(f);
        Error detail = burrow__http2_framer_error_detail(fr);
        Str got = fmt_sprintf_v(
            a, "%s|%s|%s", BURROW_FAILED(err) ? error_text(err) : BURROW_S("<nil>"),
            BURROW_FAILED(detail) ? error_text(detail) : BURROW_S("<nil>"),
            bytes_buffer_string(&tokens, a));
        Str want = str_from_cstr(tests[i].want);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%s:\n got: %q\nwant: %q", tests[i].name, got, want);
        tf_free(&tf);
    }
}

#undef P

/* What a Framer makes of an HTTP/1.1 response. */
static void TestHTTP1Response(TestingT *t) {
    static const struct {
        uint32_t max;
        const char *want;
    } tests[] = {
        {16384, "http2: failed reading the frame payload: http2: frame too large, note "
                "that the "
                "frame header looked like an HTTP/1.1 header"},
        {0,
         "http2: failed reading the frame payload: unexpected EOF, note that the frame "
         "header looked like an HTTP/1.1 header"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        BytesBuffer *r = bytes_new_buffer_string(
            heap_allocator(),
            BURROW_S("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"));
        if (r == NULL) {
            testing_t_fatalf_v(t, "out of memory");
            return;
        }
        Http2Framer *fr = burrow__http2_new_framer(heap_allocator(), (IoWriter){0},
                                                   bytes_buffer_as_io_reader(r));
        if (fr == NULL) {
            bytes_buffer_free(r);
            testing_t_fatalf_v(t, "out of memory");
            return;
        }
        if (tests[i].max != 0)
            burrow__http2_framer_set_max_read_frame_size(fr, tests[i].max);
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        burrow__http2_frame_free(f);
        Str want = str_from_cstr(tests[i].want);
        if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "%d. ReadFrame = %v; want %q", (int)i, err, want);
        burrow__http2_framer_free(fr);
        bytes_buffer_free(r);
    }
}

typedef struct LogLines {
    Alloc *a;
    Str fr;
    Str lines[8];
    int n;
} LogLines;

static void log_line(LogLines *l, const char *prefix, Str msg) {
    Str line = fmt_sprintf_v(l->a, "%s%s", prefix, msg);
    line = strings_replace(l->a, line, l->fr, BURROW_S("FR"), 1);
    if (l->n < 8)
        l->lines[l->n] = line;
    l->n++;
}

static void log_read(void *env, Str msg) {
    log_line((LogLines *)env, "R ", msg);
}

static void log_write(void *env, Str msg) {
    log_line((LogLines *)env, "W ", msg);
}

static void TestDebugLoggers(TestingT *t) {
    static const char *const want[] = {
        "W http2: Framer FR: wrote DATA flags=END_STREAM stream=1 len=3 data=\"ABC\"",
        "R http2: Framer FR: read DATA flags=END_STREAM stream=1 len=3 data=\"ABC\"",
        "W http2: Framer FR: failed to decode just-written frame",
    };
    TF tf;
    Http2Framer *fr = tf_init(&tf);
    LogLines logs = {0};
    logs.a = ta(t);
    logs.fr = fmt_sprintf_v(logs.a, "%p", (void *)fr);
    fr->log_reads = true;
    fr->log_writes = true;
    fr->debug_read_logger = BURROW_FN(Http2LogFunc, log_read, &logs);
    fr->debug_write_logger = BURROW_FN(Http2LogFunc, log_write, &logs);
    (void)burrow__http2_framer_write_data(fr, 1, true, bs("ABC"));
    Error err = BURROW_NO_ERROR;
    burrow__http2_frame_free(burrow__http2_framer_read_frame(fr, &err));
    fr->allow_illegal_writes = true;
    (void)burrow__http2_framer_write_raw_frame(fr, HTTP2_FRAME_PING, 0, 0, bs("short"));

    int nwant = (int)(sizeof want / sizeof want[0]);
    if (logs.n != nwant)
        testing_t_errorf_v(t, "got %d log lines; want %d", logs.n, nwant);
    for (int i = 0; i < logs.n && i < nwant; i++) {
        if (!str_eq(logs.lines[i], str_from_cstr(want[i])))
            testing_t_errorf_v(t, "log %d = %q; want %q", i, logs.lines[i],
                               str_from_cstr(want[i]));
    }
    tf_free(&tf);
}

static void TestMetaHeadersHelpers(TestingT *t) {
    static const Str m1[] = {BURROW_S_INIT(":method"),  BURROW_S_INIT("GET"),
                             BURROW_S_INIT(":path"),    BURROW_S_INIT("/"),
                             BURROW_S_INIT("priority"), BURROW_S_INIT("u=1"),
                             BURROW_S_INIT("foo"),      BURROW_S_INIT("bar")};
    static const Str m2[] = {BURROW_S_INIT(":method"),  BURROW_S_INIT("GET"),
                             BURROW_S_INIT(":path"),    BURROW_S_INIT("/"),
                             BURROW_S_INIT("priority"), BURROW_S_INIT("u=1"),
                             BURROW_S_INIT("via"),      BURROW_S_INIT("x")};
    static const Str m3[] = {BURROW_S_INIT(":method"), BURROW_S_INIT("GET"),
                             BURROW_S_INIT("foo"), BURROW_S_INIT("bar")};
    static const Str m4[] = {BURROW_S_INIT("foo"), BURROW_S_INIT("bar"),
                             BURROW_S_INIT("x-forwarded-for"), BURROW_S_INIT("y")};
    static const Str m5[] = {BURROW_S_INIT(":status"), BURROW_S_INIT("200"),
                             BURROW_S_INIT("priority"), BURROW_S_INIT("u=9, i")};
    static const struct {
        const Str *headers;
        const char *want;
        int n;
    } tests[] = {
        {m1,
         "method=\"GET\" path=\"/\" regular=2 pseudo=2 p1=1,0,true,false "
         "p2=1,0,true,false",
         8},
        {m2,
         "method=\"GET\" path=\"/\" regular=2 pseudo=2 p1=1,1,true,true "
         "p2=1,1,true,true",
         8},
        {m3,
         "method=\"GET\" path=\"\" regular=1 pseudo=1 p1=3,1,false,false "
         "p2=3,0,true,false",
         4},
        {m4,
         "method=\"\" path=\"\" regular=2 pseudo=0 p1=3,1,false,true p2=3,1,true,true",
         4},
        {m5,
         "method=\"\" path=\"\" regular=1 pseudo=1 p1=3,1,true,false p2=3,1,true,false",
         4},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Alloc *a = ta(t);
        BytesBuffer block = BYTES_BUFFER(a);
        Slice all = encode_header_raw(t, &block, tests[i].headers, tests[i].n);
        TF tf;
        Http2Framer *fr = tf_init(&tf);
        HpackDecoder *dec = burrow__hpack_new_decoder(
            heap_allocator(), HTTP2_INITIAL_HEADER_TABLE_SIZE, NULL, NULL);
        fr->read_meta_headers = dec;
        Http2HeadersFrameParam p = {0};
        p.stream_id = 1;
        p.block_fragment = all;
        p.end_headers = true;
        (void)burrow__http2_framer_write_headers(fr, p);
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(fr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. ReadFrame: %v", (int)i, err);
        } else {
            bool aware1 = false;
            bool inter1 = false;
            bool aware2 = false;
            bool inter2 = false;
            Http2PriorityParam p1 = burrow__http2_meta_headers_frame_rfc9218_priority(
                f, false, &aware1, &inter1);
            Http2PriorityParam p2 = burrow__http2_meta_headers_frame_rfc9218_priority(
                f, true, &aware2, &inter2);
            Str got = fmt_sprintf_v(
                a,
                "method=%q path=%q regular=%d pseudo=%d p1=%d,%d,%t,%t p2=%d,%d,%t,%t",
                burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("method")),
                burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("path")),
                burrow__http2_meta_headers_frame_regular_fields(f).len,
                burrow__http2_meta_headers_frame_pseudo_fields(f).len, p1.urgency,
                p1.incremental, aware1, inter1, p2.urgency, p2.incremental, aware2,
                inter2);
            Str want = str_from_cstr(tests[i].want);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "%d.\n got: %s\nwant: %s", (int)i, got, want);
        }
        burrow__http2_frame_free(f);
        burrow__hpack_decoder_free(dec);
        tf_free(&tf);
    }
}

#define TESTS(X)                                                                       \
    X(TestErrCodeString)                                                               \
    X(TestFrameSizes)                                                                  \
    X(TestFrameTypeString)                                                             \
    X(TestWriteRST)                                                                    \
    X(TestWriteData)                                                                   \
    X(TestWriteDataPadded)                                                             \
    X(TestWriteHeaders)                                                                \
    X(TestWriteInvalidStreamDep)                                                       \
    X(TestWriteContinuation)                                                           \
    X(TestParseRFC9218Priority)                                                        \
    X(TestWritePriority)                                                               \
    X(TestWritePriorityUpdate)                                                         \
    X(TestWriteSettings)                                                               \
    X(TestWriteSettingsAck)                                                            \
    X(TestWriteWindowUpdate)                                                           \
    X(TestWritePing)                                                                   \
    X(TestWritePingAck)                                                                \
    X(TestReadFrameHeader)                                                             \
    X(TestReadWriteFrameHeader)                                                        \
    X(TestWriteTooLargeFrame)                                                          \
    X(TestWriteGoAway)                                                                 \
    X(TestWritePushPromise)                                                            \
    X(TestReadFrameOrder)                                                              \
    X(TestMetaFrameHeader)                                                             \
    X(TestSetReuseFrames)                                                              \
    X(TestSetReuseFramesMoreThanOnce)                                                  \
    X(TestNoSetReuseFrames)                                                            \
    X(TestSettingsDuplicates)                                                          \
    X(TestTypeFrameParser)                                                             \
    X(TestReadFrameHeaderAndBody)                                                      \
    X(TestReadFrameHeaderFrameTooLarge)                                                \
    X(TestReadFrameHeaderBadFrameOrder)                                                \
    X(TestReadFrameForHeaderUnexpectedEOF)                                             \
    X(TestTypeFrameParserHolePanic)                                                    \
    X(TestErrorStrings)                                                                \
    X(TestSettingString)                                                               \
    X(TestFrameHeaderString)                                                           \
    X(TestSummarizeFrame)                                                              \
    X(TestMalformedFrames)                                                             \
    X(TestHTTP1Response)                                                               \
    X(TestDebugLoggers)                                                                \
    X(TestMetaHeadersHelpers)

TESTING_MAIN(TESTS)
