/* Derived from Go's src/compress/flate/flate_test.go, inflate_test.go,
 * reader_test.go and dict_decoder_test.go, the reader side of them.
 * Go source: go1.27.1.
 *
 * Go's tests make their compressed input with NewWriter as they go. This
 * library has no writer yet, so the streams those tests need were made by Go's
 * writer and are in tests/flate_test_gen.h, from tools/gen-flate-tests.sh. The
 * tests after TestDictDecoder are new: whole streams at every level, a reader
 * without ReadByte, reads a byte at a time, the error types, the ReadByte
 * method sets, and running out of memory.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/compress/flate_internal.h"

#include "burrow/burrow.h"
#include "burrow/compress/flate.h"
#include "burrow/encoding/base64.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"

#include "flate_test_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BS(lit)                                                                        \
    slice_from((void *)(uintptr_t)(lit), (Int)sizeof(lit) - 1, (Int)sizeof(lit) - 1,   \
               TYPE_BYTE)

static const Slice no_dict = {NULL, 0, 0, NULL};

static Str str_of(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static Int rd(IoReader r, Slice p, Error *err) {
    return r.vt->read(r.data, p, err);
}

static Error close_rc(IoReadCloser rc) {
    return rc.vt->closer.close(rc.data);
}

static Str str_sub(Str s, Int lo, Int hi) {
    return str_from_bytes(s.p + lo, hi - lo);
}

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* A generated stream, joined and decoded. */
static Slice gen_bytes(Alloc *a, const char *const *lines) {
    Int n = 0;
    for (const char *const *l = lines; *l != NULL; l++)
        n += (Int)strlen(*l);
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    Int off = 0;
    for (const char *const *l = lines; *l != NULL; l++) {
        size_t k = strlen(*l);
        memcpy(p + off, *l, k);
        off += (Int)k;
    }
    Error err;
    Slice out = base64_encoding_decode_string(base64_std_encoding, a,
                                              str_from_bytes(p, n), &err);
    if (BURROW_FAILED(err)) {
        fprintf(stderr, "bad generated stream\n");
        abort();
    }
    return out;
}

static Slice hex_bytes(Alloc *a, const char *hex) {
    Int n = (Int)strlen(hex) / 2;
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 1, 1);
    for (Int i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        p[i] = (Byte)v;
    }
    return slice_from(p, n, n, TYPE_BYTE);
}

static const char hexdigits[] = "0123456789abcdef";

static Str hex_of(Alloc *a, Slice b) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)b.len * 2 + 1, 1);
    const Byte *s = (const Byte *)b.p;
    for (Int i = 0; i < b.len; i++) {
        p[2 * i] = (Byte)hexdigits[s[i] >> 4];
        p[2 * i + 1] = (Byte)hexdigits[s[i] & 15];
    }
    return str_from_bytes(p, b.len * 2);
}

/* io.ReadAll of a reader over the decompressed form of in. */
static Slice inflate_all(Alloc *a, Slice in, Error *err) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    IoReadCloser rc = flate_new_reader(a, bytes_reader_as_io_reader(&br));
    Slice out = io_read_all(a, io_read_closer_as_io_reader(rc), err);
    flate_reader_free(rc);
    return out;
}

/* ------------------------------------------------------- flate_test.go */

static void TestIssue5915(TestingT *t) {
    static const int bits[] = {4, 0, 0, 6, 4, 3, 2, 3,  3, 4, 4, 5, 0, 0,  0,  0,
                               5, 5, 6, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0,  11, 0,
                               0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0,  0,  0,
                               0, 0, 0, 7, 8, 6, 0, 11, 0, 8, 0, 6, 6, 10, 8};
    if (burrow__flate_huff_init_ok(bits, (int)(sizeof bits / sizeof bits[0])))
        testing_t_fatalf_v(t, "Given sequence of bits is bad, and should not succeed.");
}

static void TestIssue5962(TestingT *t) {
    static const int bits[] = {4, 0, 0, 6, 4, 3, 2, 3, 3, 4, 4, 5, 0, 0, 0,
                               0, 5, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 11};
    if (burrow__flate_huff_init_ok(bits, (int)(sizeof bits / sizeof bits[0])))
        testing_t_fatalf_v(t, "Given sequence of bits is bad, and should not succeed.");
}

static void TestIssue6255(TestingT *t) {
    static const int bits1[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 11};
    static const int bits2[] = {11, 13};
    if (!burrow__flate_huff_init_ok(bits1, 12))
        testing_t_fatalf_v(t, "Given sequence of bits is good and should succeed.");
    if (burrow__flate_huff_init_ok(bits2, 2))
        testing_t_fatalf_v(t, "Given sequence of bits is bad and should not succeed.");
}

static void TestInvalidEncoding(TestingT *t) {
    /* Initialize Huffman decoder to recognize "0", and give it a 1. */
    static const int bits[] = {1};
    Byte in[] = {0xff};
    BytesReader br;
    bytes_reader_reset(&br, slice_from(in, 1, 1, TYPE_BYTE));
    Error err = burrow__flate_huff_sym(bits, 1, bytes_reader_as_io_reader(&br));
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "Should have rejected invalid bit sequence");
    CHECK(errors_as(err, TYPE_FLATE_CORRUPT_INPUT_ERROR) != NULL);
}

static void TestInvalidBits(TestingT *t) {
    static const int oversubscribed[] = {1, 2, 3, 4, 4, 5};
    static const int incomplete[] = {1, 2, 4, 4};
    if (burrow__flate_huff_init_ok(oversubscribed, 6))
        testing_t_fatalf_v(t, "Should reject oversubscribed bit-length set");
    if (burrow__flate_huff_init_ok(incomplete, 4))
        testing_t_fatalf_v(t, "Should reject incomplete bit-length set");
}

typedef struct StreamCase {
    const char *desc;   /* Description of the stream */
    const char *stream; /* Hexstring of the input DEFLATE stream */
    const char *want;   /* Expected result. Use "fail" to expect failure */
} StreamCase;

static const StreamCase stream_cases[] = {
    {"degenerate HCLenTree",
     "05e0010000000000100000000000000000000000000000000000000000000000"
     "00000000000000000004",
     "fail"},
    {"complete HCLenTree, empty HLitTree, empty HDistTree",
     "05e0010400000000000000000000000000000000000000000000000000000000"
     "00000000000000000010",
     "fail"},
    {"empty HCLenTree",
     "05e0010000000000000000000000000000000000000000000000000000000000"
     "00000000000000000010",
     "fail"},
    {"complete HCLenTree, complete HLitTree, empty HDistTree, use missing HDist symbol",
     "000100feff000de0010400000000100000000000000000000000000000000000"
     "0000000000000000000000000000002c",
     "fail"},
    {"complete HCLenTree, complete HLitTree, degenerate HDistTree, use missing HDist "
     "symbol",
     "000100feff000de0010000000000000000000000000000000000000000000000"
     "00000000000000000610000000004070",
     "fail"},
    {"complete HCLenTree, empty HLitTree, empty HDistTree",
     "05e0010400000000100400000000000000000000000000000000000000000000"
     "0000000000000000000000000008",
     "fail"},
    {"complete HCLenTree, empty HLitTree, degenerate HDistTree",
     "05e0010400000000100400000000000000000000000000000000000000000000"
     "0000000000000000000800000008",
     "fail"},
    {"complete HCLenTree, degenerate HLitTree, degenerate HDistTree, use missing HLit "
     "symbol",
     "05e0010400000000100000000000000000000000000000000000000000000000"
     "0000000000000000001c",
     "fail"},
    {"complete HCLenTree, complete HLitTree, too large HDistTree",
     "edff870500000000200400000000000000000000000000000000000000000000"
     "000000000000000000080000000000000004",
     "fail"},
    {"complete HCLenTree, complete HLitTree, empty HDistTree, excessive repeater code",
     "edfd870500000000200400000000000000000000000000000000000000000000"
     "000000000000000000e8b100",
     "fail"},
    {"complete HCLenTree, complete HLitTree, empty HDistTree of normal length 30",
     "05fd01240000000000f8ffffffffffffffffffffffffffffffffffffffffffff"
     "ffffffffffffffffff07000000fe01",
     ""},
    {"complete HCLenTree, complete HLitTree, empty HDistTree of excessive length 31",
     "05fe01240000000000f8ffffffffffffffffffffffffffffffffffffffffffff"
     "ffffffffffffffffff07000000fc03",
     "fail"},
    {"complete HCLenTree, over-subscribed HLitTree, empty HDistTree",
     "05e001240000000000fcffffffffffffffffffffffffffffffffffffffffffff"
     "ffffffffffffffffff07f00f",
     "fail"},
    {"complete HCLenTree, under-subscribed HLitTree, empty HDistTree",
     "05e001240000000000fcffffffffffffffffffffffffffffffffffffffffffff"
     "fffffffffcffffffff07f00f",
     "fail"},
    {"complete HCLenTree, complete HLitTree with single code, empty HDistTree",
     "05e001240000000000f8ffffffffffffffffffffffffffffffffffffffffffff"
     "ffffffffffffffffff07f00f",
     "01"},
    {"complete HCLenTree, complete HLitTree with multiple codes, empty HDistTree",
     "05e301240000000000f8ffffffffffffffffffffffffffffffffffffffffffff"
     "ffffffffffffffffff07807f",
     "01"},
    {"complete HCLenTree, complete HLitTree, degenerate HDistTree, use valid HDist "
     "symbol",
     "000100feff000de0010400000000100000000000000000000000000000000000"
     "0000000000000000000000000000003c",
     "00000000"},
    {"complete HCLenTree, degenerate HLitTree, degenerate HDistTree",
     "05e0010400000000100000000000000000000000000000000000000000000000"
     "0000000000000000000c",
     ""},
    {"complete HCLenTree, degenerate HLitTree, empty HDistTree",
     "05e0010400000000100000000000000000000000000000000000000000000000"
     "00000000000000000004",
     ""},
    {"complete HCLenTree, complete HLitTree, empty HDistTree, spanning repeater code",
     "edfd870500000000200400000000000000000000000000000000000000000000"
     "000000000000000000e8b000",
     ""},
    {"complete HCLenTree with length codes, complete HLitTree, empty HDistTree",
     "ede0010400000000100000000000000000000000000000000000000000000000"
     "0000000000000000000400004000",
     ""},
    {"complete HCLenTree, complete HLitTree, degenerate HDistTree, use valid HLit "
     "symbol 284 with count 31",
     "000100feff00ede0010400000000100000000000000000000000000000000000"
     "000000000000000000000000000000040000407f00",
     "0000000000000000000000000000000000000000000000000000000000000000"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "0000000000000000000000000000000000000000000000000000000000000000"
     "000000"},
    {"complete HCLenTree, complete HLitTree, degenerate HDistTree, use valid HLit and "
     "HDist symbols",
     "0cc2010d00000082b0ac4aff0eb07d27060000ffff", "616263616263"},
    {"fixed block, use reserved symbol 287", "33180700", "fail"},
    {"raw block", "010100feff11", "11"},
    {"issue 10426 - over-subscribed HCLenTree causes a hang",
     "344c4a4e494d4b070000ff2e2eff2e2e2e2e2eff", "fail"},
    {"issue 11030 - empty HDistTree unexpectedly leads to error",
     "05c0070600000080400fff37a0ca", ""},
    {"issue 11033 - empty HDistTree unexpectedly leads to error",
     "050fb109c020cca5d017dcbca044881ee1034ec149c8980bbc413c2ab35be9dc"
     "b1473449922449922411202306ee97b0383a521b4ffdcf3217f9f7d3adb701",
     "3130303634342068652e706870005d05355f7ed957ff084a90925d19e3ebc6d0"
     "c6d7"},
};

static void TestStreams(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof stream_cases / sizeof stream_cases[0]; i++) {
        const StreamCase *tc = &stream_cases[i];
        Error err;
        Slice data = inflate_all(a, hex_bytes(a, tc->stream), &err);
        if (strcmp(tc->want, "fail") == 0) {
            if (BURROW_OK(err))
                testing_t_errorf_v(t, "#%d (%s): got nil error, want non-nil", (int)i,
                                   tc->desc);
        } else {
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d (%s): %s", (int)i, tc->desc,
                                   error_text(err));
                continue;
            }
            Str got = hex_of(a, data);
            if (!str_eq(got, str_from_cstr(tc->want)))
                testing_t_errorf_v(t, "#%d (%s):\ngot  %q\nwant %q", (int)i, tc->desc,
                                   got, tc->want);
        }
    }
    arena_free(&ar);
}

static void TestTruncatedStreams(TestingT *t) {
    static const char data[] = "\x00\f\x00\xf3\xffhello, world\x01\x00\x00\xff\xff";
    Int len = (Int)sizeof data - 1;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < len - 1; i++) {
        StringsReader sr;
        strings_reader_reset(&sr, str_from_bytes((const Byte *)data, i));
        IoReadCloser r = flate_new_reader(a, strings_reader_as_io_reader(&sr));
        Error err;
        io_copy(a, io_discard, io_read_closer_as_io_reader(r), &err);
        if (!same_error(err, io_err_unexpected_eof))
            testing_t_errorf_v(t, "io.Copy(%d) on truncated stream: got %s, want %s",
                               (int)i, error_text(err),
                               error_text(io_err_unexpected_eof));
        flate_reader_free(r);
    }
    arena_free(&ar);
}

enum { WINDOW_SIZE = 1 << 15 };

static void TestReaderEarlyEOF(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte read_buf[40];
    for (size_t k = 0; k < sizeof flate_early / sizeof flate_early[0]; k++) {
        const FlateEarly *e = &flate_early[k];
        if (testing_short() && e->size > WINDOW_SIZE)
            continue;
        for (int pass = 0; pass < 2; pass++) {
            bool flush = pass == 0;
            /* If a Flush occurs after all the actual data, the flushing
             * semantics dictate that we will observe a (0, io.EOF) since Read
             * must return data before it knows that the stream ended. */
            bool early_eof = !flush;
            Slice in = gen_bytes(a, flush ? e->flushed : e->plain);
            BytesReader br;
            bytes_reader_reset(&br, in);
            IoReadCloser r = flate_new_reader(a, bytes_reader_as_io_reader(&br));
            IoReader rr = io_read_closer_as_io_reader(r);
            Int total = 0;
            for (;;) {
                Error err;
                Int n = rd(rr, slice_from(read_buf, 40, 40, TYPE_BYTE), &err);
                for (Int i = 0; i < n; i++) {
                    if (read_buf[i] != (Byte)(total + i)) {
                        testing_t_fatalf_v(t, "size %d: byte %d is %d", (int)e->size,
                                           (int)(total + i), read_buf[i]);
                    }
                }
                total += n;
                if (same_error(err, io_eof)) {
                    /* If the availWrite == windowSize, then that means that
                     * the previous Read returned because the write buffer was
                     * full and it just so happened that the stream had no
                     * more data. This situation is rare, but unavoidable. */
                    if (burrow__flate_avail_write(r) == WINDOW_SIZE)
                        early_eof = false;
                    if (n == 0 && early_eof)
                        testing_t_errorf_v(t,
                                           "On size:%d flush:%t, Read() = (0, io.EOF), "
                                           "want (n, io.EOF)",
                                           (int)e->size, flush);
                    if (n != 0 && !early_eof)
                        testing_t_errorf_v(
                            t,
                            "On size:%d flush:%t, Read() = (%d, io.EOF), "
                            "want (0, io.EOF)",
                            (int)e->size, flush, (int)n);
                    break;
                }
                if (BURROW_FAILED(err))
                    testing_t_fatalf_v(t, "%s", error_text(err));
            }
            CHECK_INT_EQ(total, e->size);
            flate_reader_free(r);
        }
    }
    arena_free(&ar);
}

/* ----------------------------------------------------- inflate_test.go */

static const char *const reset_inputs[] = {
    "lorem ipsum izzle fo rizzle",
    "the quick brown fox jumped over",
};

static void TestReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const char *const *streams[2] = {reset_0, reset_1};
    BytesBuffer *deflated[2], *inflated[2];
    for (int i = 0; i < 2; i++) {
        deflated[i] = bytes_new_buffer(a, gen_bytes(a, streams[i]));
        inflated[i] = bytes_new_buffer(a, no_dict);
    }

    IoReadCloser f = flate_new_reader(a, bytes_buffer_as_io_reader(deflated[0]));
    io_copy(a, bytes_buffer_as_io_writer(inflated[0]), io_read_closer_as_io_reader(f),
            NULL);
    FlateResetter rs = flate_reader_as_resetter(f);
    CHECK(rs.vt != NULL);
    flate_resetter_reset(rs, bytes_buffer_as_io_reader(deflated[1]), no_dict);
    io_copy(a, bytes_buffer_as_io_writer(inflated[1]), io_read_closer_as_io_reader(f),
            NULL);
    CHECK(BURROW_OK(close_rc(f)));

    for (int i = 0; i < 2; i++) {
        Str got = str_of(bytes_buffer_bytes(inflated[i]));
        if (!str_eq(got, str_from_cstr(reset_inputs[i])))
            testing_t_errorf_v(t, "inflated[%d]:\ngot  %q\nwant %q", i, got,
                               reset_inputs[i]);
    }
    flate_reader_free(f);
    arena_free(&ar);
}

typedef struct TruncVector {
    const char *input;
    Int input_len;
    const char *output;
    Int output_len;
} TruncVector;

#define TV(in, out) {in, (Int)sizeof(in) - 1, out, (Int)sizeof(out) - 1}

static void TestReaderTruncated(TestingT *t) {
    static const TruncVector vectors[] = {
        TV("\x00", ""),
        TV("\x00\f", ""),
        TV("\x00\f\x00", ""),
        TV("\x00\f\x00\xf3\xff", ""),
        TV("\x00\f\x00\xf3\xffhello", "hello"),
        TV("\x00\f\x00\xf3\xffhello, world", "hello, world"),
        TV("\x02", ""),
        TV("\xf2H\xcd", "He"),
        TV("\xf2H\xcd\x99"
           "0a\xc2\x84\t",
           "Hel\x90\x90\x90\x90\x90"),
        TV("\xf2H\xcd\x99"
           "0a\xc2\x84\t\x00",
           "Hel\x90\x90\x90\x90\x90"),
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; i++) {
        const TruncVector *v = &vectors[i];
        StringsReader sr;
        strings_reader_reset(&sr, str_from_bytes((const Byte *)v->input, v->input_len));
        IoReadCloser zr = flate_new_reader(a, strings_reader_as_io_reader(&sr));
        Error err;
        Slice b = io_read_all(a, io_read_closer_as_io_reader(zr), &err);
        if (!same_error(err, io_err_unexpected_eof))
            testing_t_errorf_v(
                t, "test %d, error mismatch: got %s, want io.ErrUnexpectedEOF", (int)i,
                error_text(err));
        Str want = str_from_bytes((const Byte *)v->output, v->output_len);
        if (!str_eq(str_of(b), want))
            testing_t_errorf_v(t, "test %d, output mismatch: got %q, want %q", (int)i,
                               str_of(b), want);
        flate_reader_free(zr);
    }
    arena_free(&ar);
}

static void TestResetDict(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice dict = BS("the lorem fox");
    const char *const *streams[2] = {reset_dict_0, reset_dict_1};
    BytesBuffer *inflated[2];

    IoReadCloser f = flate_new_reader(a, (IoReader){NULL, NULL});
    for (int i = 0; i < 2; i++) {
        BytesBuffer *deflated = bytes_new_buffer(a, gen_bytes(a, streams[i]));
        inflated[i] = bytes_new_buffer(a, no_dict);
        flate_resetter_reset(flate_reader_as_resetter(f),
                             bytes_buffer_as_io_reader(deflated), dict);
        io_copy(a, bytes_buffer_as_io_writer(inflated[i]),
                io_read_closer_as_io_reader(f), NULL);
    }
    close_rc(f);

    for (int i = 0; i < 2; i++) {
        Str got = str_of(bytes_buffer_bytes(inflated[i]));
        if (!str_eq(got, str_from_cstr(reset_inputs[i])))
            testing_t_errorf_v(t, "inflated[%d]:\ngot  %q\nwant %q", i, got,
                               reset_inputs[i]);
    }
    flate_reader_free(f);
    arena_free(&ar);
}

/* An io.Reader and nothing else, like Go's struct{ io.Reader }. */
typedef struct PlainReader {
    IoReader r;
} PlainReader;

static Int plain_read(void *self, Slice p, Error *err) {
    return rd(((PlainReader *)self)->r, p, err);
}

static const IoReaderVT plain_reader_vt = {NULL, plain_read};

static IoReader plain_of(PlainReader *p) {
    return (IoReader){&plain_reader_vt, p};
}

static bool is_bufio(IoReader r) {
    return r.vt != NULL && r.vt->self_type == TYPE_BUFIO_READER;
}

static void TestReaderReusesReaderBuffer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader encoded;
    bytes_reader_reset(&encoded, no_dict);
    IoReader encoded_reader = bytes_reader_as_io_reader(&encoded);
    PlainReader plain = {encoded_reader};
    IoReader encoded_not_byte_reader = plain_of(&plain);

    /* BufferIsReused */
    {
        IoReadCloser f = flate_new_reader(a, encoded_not_byte_reader);
        IoReader bufio_r = burrow__flate_source(f);
        if (!is_bufio(bufio_r))
            testing_t_fatalf_v(t, "bufio.Reader should be created");
        flate_resetter_reset(flate_reader_as_resetter(f), encoded_not_byte_reader,
                             no_dict);
        if (bufio_r.data != burrow__flate_source(f).data)
            testing_t_fatalf_v(t, "bufio.Reader was not reused");
        flate_reader_free(f);
    }
    /* BufferIsNotReusedWhenGotByteReader */
    {
        IoReadCloser f = flate_new_reader(a, encoded_not_byte_reader);
        if (!is_bufio(burrow__flate_source(f)))
            testing_t_fatalf_v(t, "bufio.Reader should be created");
        flate_resetter_reset(flate_reader_as_resetter(f), encoded_reader, no_dict);
        if (burrow__flate_source(f).data != (void *)&encoded)
            testing_t_fatalf_v(t, "provided io.ByteReader should be used directly");
        flate_reader_free(f);
    }
    /* BufferIsCreatedAfterByteReader */
    {
        BufioReader *br = bufio_new_reader(a, encoded_reader);
        IoReader rs[2] = {encoded_reader, bufio_reader_as_io_reader(br)};
        for (int i = 0; i < 2; i++) {
            IoReadCloser f = flate_new_reader(a, rs[i]);
            if (burrow__flate_source(f).data != rs[i].data)
                testing_t_fatalf_v(
                    t, "provided io.ByteReader should be used directly, i=%d", i);
            flate_resetter_reset(flate_reader_as_resetter(f), encoded_not_byte_reader,
                                 no_dict);
            IoReader src = burrow__flate_source(f);
            if (!is_bufio(src) || src.data == (void *)br)
                testing_t_fatalf_v(t, "bufio.Reader should be created, i=%d", i);
            flate_reader_free(f);
        }
        bufio_reader_free(br);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------ reader_test.go */

static void TestNlitOutOfRange(TestingT *t) {
    /* Trying to decode this bogus flate data, which has a Huffman table with
     * nlit=288, should not panic. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice in = BS("\xfc\xfe\x36\xe7\x5e\x1c\xef\xb3\x55\x58\x77\xb6\x56\xb5\x43\xf4"
                  "\x6f\xf2\xd2\xe6\x3d\x99\xa0\x85\x8c\x48\xeb\xf8\xda\x83\x04\x2a"
                  "\x75\xc4\xf8\x0f\x12\x11\xb9\xb4\x4b\x09\xa0\xbe\x8b\x91\x4c");
    Error err;
    inflate_all(a, in, &err);
    CHECK(errors_as(err, TYPE_FLATE_CORRUPT_INPUT_ERROR) != NULL);
    arena_free(&ar);
}

/* ------------------------------------------------ dict_decoder_test.go */

typedef struct DictTest {
    FlateDict dd;
    BytesBuffer *got;
} DictTest;

static void dict_write_copy(DictTest *d, Int dist, Int length) {
    while (length > 0) {
        Int cnt = flate_dict_try_write_copy(&d->dd, dist, length);
        if (cnt == 0)
            cnt = flate_dict_write_copy(&d->dd, dist, length);
        length -= cnt;
        if (flate_dict_avail_write(&d->dd) == 0)
            bytes_buffer_write(d->got, flate_dict_read_flush(&d->dd), NULL);
    }
}

static void dict_write_string(DictTest *d, Str s) {
    while (s.len > 0) {
        Int cnt = flate_dict_avail_write(&d->dd);
        if (cnt > s.len)
            cnt = s.len;
        memcpy(d->dd.hist + d->dd.wr_pos, s.p, (size_t)cnt);
        s = str_sub(s, cnt, s.len);
        d->dd.wr_pos += cnt;
        if (flate_dict_avail_write(&d->dd) == 0)
            bytes_buffer_write(d->got, flate_dict_read_flush(&d->dd), NULL);
    }
}

static void want_repeat(BytesBuffer *want, Str s, int n) {
    for (int i = 0; i < n; i++)
        bytes_buffer_write_string(want, s, NULL);
}

static Str upper_of(Alloc *a, Str s) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)s.len + 1, 1);
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        p[i] = c >= 'a' && c <= 'z' ? (Byte)(c - 'a' + 'A') : c;
    }
    return str_from_bytes(p, s.len);
}

static void TestDictDecoder(TestingT *t) {
    static const char abc[] = "ABC\n";
    static const char fox[] = "The quick brown fox jumped over the lazy dog!\n";
    static const char poem[] = "The Road Not Taken\nRobert Frost\n"
                               "\n"
                               "Two roads diverged in a yellow wood,\n"
                               "And sorry I could not travel both\n"
                               "And be one traveler, long I stood\n"
                               "And looked down one as far as I could\n"
                               "To where it bent in the undergrowth;\n"
                               "\n"
                               "Then took the other, as just as fair,\n"
                               "And having perhaps the better claim,\n"
                               "Because it was grassy and wanted wear;\n"
                               "Though as for that the passing there\n"
                               "Had worn them really about the same,\n"
                               "\n"
                               "And both that morning equally lay\n"
                               "In leaves no step had trodden black.\n"
                               "Oh, I kept the first for another day!\n"
                               "Yet knowing how way leads on to way,\n"
                               "I doubted if I should ever come back.\n"
                               "\n"
                               "I shall be telling this with a sigh\n"
                               "Somewhere ages and ages hence:\n"
                               "Two roads diverged in a wood, and I-\n"
                               "I took the one less traveled by,\n"
                               "And that has made all the difference.\n";

    /* dist is the backward distance (0 if this is an insertion) and length
     * the length of the copy or insertion. */
    static const int poem_refs[][2] = {
        {0, 38},  {33, 3},   {0, 48},  {79, 3},   {0, 11},  {34, 5},  {0, 6},
        {23, 7},  {0, 8},    {50, 3},  {0, 2},    {69, 3},  {34, 5},  {0, 4},
        {97, 3},  {0, 4},    {43, 5},  {0, 6},    {7, 4},   {88, 7},  {0, 12},
        {80, 3},  {0, 2},    {141, 4}, {0, 1},    {196, 3}, {0, 3},   {157, 3},
        {0, 6},   {181, 3},  {0, 2},   {23, 3},   {77, 3},  {28, 5},  {128, 3},
        {110, 4}, {70, 3},   {0, 4},   {85, 6},   {0, 2},   {182, 6}, {0, 4},
        {133, 3}, {0, 7},    {47, 5},  {0, 20},   {112, 5}, {0, 1},   {58, 3},
        {0, 8},   {59, 3},   {0, 4},   {173, 3},  {0, 5},   {114, 3}, {0, 4},
        {92, 5},  {0, 2},    {71, 3},  {0, 2},    {76, 5},  {0, 1},   {46, 3},
        {96, 4},  {130, 4},  {0, 3},   {360, 3},  {0, 3},   {178, 5}, {0, 7},
        {75, 3},  {0, 3},    {45, 6},  {0, 6},    {299, 6}, {180, 3}, {70, 6},
        {0, 1},   {48, 3},   {66, 4},  {0, 3},    {47, 5},  {0, 9},   {325, 3},
        {0, 1},   {359, 3},  {318, 3}, {0, 2},    {199, 3}, {0, 1},   {344, 3},
        {0, 3},   {248, 3},  {0, 10},  {310, 3},  {0, 3},   {93, 6},  {0, 3},
        {252, 3}, {157, 4},  {0, 2},   {273, 5},  {0, 14},  {99, 4},  {0, 1},
        {464, 4}, {0, 2},    {92, 4},  {495, 3},  {0, 1},   {322, 4}, {16, 4},
        {0, 3},   {402, 3},  {0, 2},   {237, 4},  {0, 2},   {432, 4}, {0, 1},
        {483, 5}, {0, 2},    {294, 4}, {0, 2},    {306, 3}, {113, 5}, {0, 1},
        {26, 4},  {164, 3},  {488, 4}, {0, 1},    {542, 3}, {248, 6}, {0, 5},
        {205, 3}, {0, 8},    {48, 3},  {449, 6},  {0, 2},   {192, 3}, {328, 4},
        {9, 5},   {433, 3},  {0, 3},   {622, 25}, {615, 5}, {46, 5},  {0, 2},
        {104, 3}, {475, 10}, {549, 3}, {0, 4},    {597, 8}, {314, 3}, {0, 1},
        {473, 6}, {317, 5},  {0, 1},   {400, 3},  {0, 3},   {109, 3}, {151, 3},
        {48, 4},  {0, 4},    {125, 3}, {108, 3},  {0, 2},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DictTest d;
    d.got = bytes_new_buffer(a, no_dict);
    BytesBuffer *want = bytes_new_buffer(a, no_dict);
    d.dd.size = 1 << 11;
    d.dd.hist = (Byte *)mem_alloc(a, (size_t)d.dd.size, 1);
    flate_dict_init(&d.dd, no_dict);

    dict_write_string(&d, BURROW_S("."));
    bytes_buffer_write_byte(want, '.');

    Str s = str_from_cstr(poem);
    for (size_t i = 0; i < sizeof poem_refs / sizeof poem_refs[0]; i++) {
        Int dist = poem_refs[i][0], length = poem_refs[i][1];
        if (dist == 0)
            dict_write_string(&d, str_sub(s, 0, length));
        else
            dict_write_copy(&d, dist, length);
        s = str_sub(s, length, s.len);
    }
    bytes_buffer_write_string(want, str_from_cstr(poem), NULL);

    dict_write_copy(&d, flate_dict_hist_size(&d.dd), 33);
    bytes_buffer_write(want, slice_sub(bytes_buffer_bytes(want), 0, 33), NULL);

    dict_write_string(&d, str_from_cstr(abc));
    dict_write_copy(&d, (Int)strlen(abc), 59 * (Int)strlen(abc));
    want_repeat(want, str_from_cstr(abc), 60);

    dict_write_string(&d, str_from_cstr(fox));
    dict_write_copy(&d, (Int)strlen(fox), 9 * (Int)strlen(fox));
    want_repeat(want, str_from_cstr(fox), 10);

    dict_write_string(&d, BURROW_S("."));
    dict_write_copy(&d, 1, 9);
    want_repeat(want, BURROW_S("."), 10);

    Str up = upper_of(a, str_from_cstr(poem));
    dict_write_string(&d, up);
    dict_write_copy(&d, up.len, 7 * up.len);
    want_repeat(want, up, 8);

    Int hs = flate_dict_hist_size(&d.dd);
    dict_write_copy(&d, hs, 10);
    Slice wb = bytes_buffer_bytes(want);
    Byte copy10[10];
    for (Int i = 0; i < 10; i++)
        copy10[i] = *(const Byte *)slice_at(wb, wb.len - hs + i);
    bytes_buffer_write(want, slice_from(copy10, 10, 10, TYPE_BYTE), NULL);

    bytes_buffer_write(d.got, flate_dict_read_flush(&d.dd), NULL);
    Str got_s = str_of(bytes_buffer_bytes(d.got));
    Str want_s = str_of(bytes_buffer_bytes(want));
    if (!str_eq(got_s, want_s))
        testing_t_errorf_v(t, "final string mismatch:\ngot  %q\nwant %q", got_s,
                           want_s);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ new */

/* The words tools/gen-flate-tests.sh compresses, made the same way. */
static Slice flate_words(Alloc *a, Int n) {
    static const char *const vocab[] = {
        "the",  "quick", "brown", "fox",  "jumped", "over",  "lazy",  "dog",
        "and",  "a",     "of",    "to",   "in",     "is",    "that",  "it",
        "was",  "for",   "on",    "are",  "as",     "with",  "his",   "they",
        "at",   "be",    "this",  "from", "I",      "have",  "or",    "by",
        "one",  "had",   "not",   "but",  "what",   "all",   "were",  "when",
        "we",   "there", "can",   "an",   "your",   "which", "their", "said",
        "if",   "do",    "will",  "each", "about",  "how",   "up",    "out",
        "them", "then",  "she",   "many", "some",   "so",    "these", "would",
    };
    const uint64_t nv = sizeof vocab / sizeof vocab[0];
    Byte *p = (Byte *)mem_alloc(a, (size_t)n + 16, 1);
    Int len = 0;
    uint64_t x = 1;
    while (len < n) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        const char *w = vocab[(x >> 33) % nv];
        size_t k = strlen(w);
        for (size_t i = 0; i < k && len < n; i++)
            p[len++] = (Byte)w[i];
        if (len < n)
            p[len++] = x >> 60 == 0 ? '\n' : ' ';
    }
    return slice_from(p, n, n, TYPE_BYTE);
}

typedef enum { VIA_BYTES, VIA_PLAIN, VIA_BUFIO, VIA_STRINGS } Via;

/* Decompresses in through the kind of reader via says, reading step bytes at
 * a time, and checks the result is want. */
static void check_stream(TestingT *t, Alloc *a, const char *name, Slice in, Slice dict,
                         Slice want, Via via, Int step) {
    BytesReader br;
    bytes_reader_reset(&br, in);
    StringsReader sr;
    strings_reader_reset(&sr, str_of(in));
    PlainReader plain = {bytes_reader_as_io_reader(&br)};
    BufioReader *bu = NULL;
    IoReader src;
    switch (via) {
    case VIA_PLAIN:
        src = plain_of(&plain);
        break;
    case VIA_BUFIO:
        bu = bufio_new_reader_size(a, plain_of(&plain), 16);
        src = bufio_reader_as_io_reader(bu);
        break;
    case VIA_STRINGS:
        src = strings_reader_as_io_reader(&sr);
        break;
    case VIA_BYTES:
    default:
        src = bytes_reader_as_io_reader(&br);
        break;
    }
    IoReadCloser rc = flate_new_reader_dict(a, src, dict);
    IoReader r = io_read_closer_as_io_reader(rc);
    Byte *out = (Byte *)mem_alloc(a, (size_t)want.len + 1, 1);
    Int total = 0;
    Error err = BURROW_NO_ERROR;
    Byte scratch[1];
    for (;;) {
        Slice p = total < want.len
                      ? slice_from(out + total,
                                   want.len - total < step ? want.len - total : step,
                                   want.len - total, TYPE_BYTE)
                      : slice_from(scratch, 1, 1, TYPE_BYTE);
        Int n = rd(r, p, &err);
        if (total >= want.len && n > 0) {
            testing_t_errorf_v(t, "%s via %d step %d: more output than %d bytes", name,
                               (int)via, (int)step, (int)want.len);
            break;
        }
        total += n;
        if (BURROW_FAILED(err))
            break;
    }
    if (!same_error(err, io_eof))
        testing_t_errorf_v(t, "%s via %d step %d: error %s", name, (int)via, (int)step,
                           error_text(err));
    else if (total != want.len || memcmp(out, want.p, (size_t)want.len) != 0) {
        Int d = 0;
        while (d < total && d < want.len && out[d] == ((const Byte *)want.p)[d])
            d++;
        testing_t_errorf_v(
            t, "%s via %d step %d: got %d bytes that differ at %d from the %d wanted",
            name, (int)via, (int)step, (int)total, (int)d, (int)want.len);
    }
    CHECK(BURROW_OK(close_rc(rc)));
    /* With ReadByte on the input, nothing past the end of the stream is read. */
    if (via == VIA_BYTES)
        CHECK_INT_EQ(bytes_reader_len(&br), 0);
    flate_reader_free(rc);
    bufio_reader_free(bu);
}

static void TestGoStreams(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice gb = gen_bytes(a, gettysburg);
    CHECK_INT_EQ(gb.len, GETTYSBURG_LEN);
    Slice words = flate_words(a, 34000);
    static const Int steps[] = {1, 7, 4096, 1 << 20};
    for (size_t i = 0; i < sizeof flate_streams / sizeof flate_streams[0]; i++) {
        const FlateStream *s = &flate_streams[i];
        Slice in = gen_bytes(a, s->b64);
        Slice want = s->input == 0   ? gb
                     : s->input == 1 ? words
                                     : slice_sub(words, 5000, words.len);
        Slice dict = s->input == 2 ? slice_sub(words, 0, 5000) : no_dict;
        const char *name = s->input == 0   ? "gettysburg"
                           : s->input == 1 ? "words"
                                           : "words with a dictionary";
        for (int via = VIA_BYTES; via <= VIA_STRINGS; via++) {
            for (size_t k = 0; k < sizeof steps / sizeof steps[0]; k++) {
                if (steps[k] == 1 && s->input != 0 && via != VIA_BYTES)
                    continue;
                check_stream(t, a, name, in, dict, want, (Via)via, steps[k]);
            }
        }
    }
    arena_free(&ar);
}

/* Every prefix of a real stream, which has dynamic blocks in it, fails with
 * io.ErrUnexpectedEOF and hands out what it decoded first, and so does every
 * prefix read through a reader with no ReadByte. */
static void TestTruncatedGoStream(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice gb = gen_bytes(a, gettysburg);
    Slice in = gen_bytes(a, gettysburg_l8);
    for (Int i = 0; i < in.len; i++) {
        Error err;
        Slice out = inflate_all(a, slice_sub(in, 0, i), &err);
        if (!same_error(err, io_err_unexpected_eof)) {
            testing_t_errorf_v(t, "prefix %d: error %s", (int)i, error_text(err));
            continue;
        }
        if (out.len > gb.len || memcmp(out.p, gb.p, (size_t)out.len) != 0)
            testing_t_errorf_v(t, "prefix %d: output is not a prefix of the text",
                               (int)i);
    }
    arena_free(&ar);
}

/* A corrupt stream stops with the offset it had reached, hands out what came
 * before it, and keeps giving the same error. */
static void TestCorruptInputError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* A stored block holding "hi" and then block type 3. */
    Slice in = BS("\x00\x02\x00\xfd\xffhi\x07");
    BytesReader br;
    bytes_reader_reset(&br, in);
    IoReadCloser rc = flate_new_reader(a, bytes_reader_as_io_reader(&br));
    Error err;
    Slice out = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
    CHECK(str_eq(str_of(out), BURROW_S("hi")));
    const FlateCorruptInputError *off = errors_as(err, TYPE_FLATE_CORRUPT_INPUT_ERROR);
    CHECK(off != NULL && *off == 8);
    CHECK(str_eq(error_text(err), BURROW_S("flate: corrupt input before offset 8")));
    CHECK(errors_is(err, flate_corrupt_input_error_as_error(8, a)));
    CHECK(!errors_is(err, flate_corrupt_input_error_as_error(7, a)));
    Error again;
    Byte one[1];
    CHECK_INT_EQ(
        rd(io_read_closer_as_io_reader(rc), slice_from(one, 1, 1, TYPE_BYTE), &again),
        0);
    CHECK(same_error(again, err));
    CHECK(same_error(close_rc(rc), err));
    flate_reader_free(rc);

    CHECK(str_eq(flate_corrupt_input_error_error(-12, a),
                 BURROW_S("flate: corrupt input before offset -12")));
    arena_free(&ar);
}

static void TestErrorTypes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error ie = flate_internal_error_as_error(BURROW_S("oops"), a);
    CHECK(str_eq(error_text(ie), BURROW_S("flate: internal error: oops")));
    CHECK(str_eq(flate_internal_error_error(BURROW_S("oops"), a),
                 BURROW_S("flate: internal error: oops")));
    const FlateInternalError *is = errors_as(ie, TYPE_FLATE_INTERNAL_ERROR);
    CHECK(is != NULL && str_eq(*is, BURROW_S("oops")));
    CHECK(errors_is(ie, flate_internal_error_as_error(BURROW_S("oops"), a)));
    CHECK(!errors_is(ie, flate_internal_error_as_error(BURROW_S("oops!"), a)));

    FlateReadError re = {42, io_err_unexpected_eof};
    Error rerr = flate_read_error_as_error(&re, a);
    CHECK(str_eq(error_text(rerr),
                 BURROW_S("flate: read error at offset 42: unexpected EOF")));
    CHECK(str_eq(flate_read_error_error(&re, a), error_text(rerr)));
    const FlateReadError *rp = errors_as(rerr, TYPE_FLATE_READ_ERROR);
    CHECK(rp != NULL && rp->offset == 42 && same_error(rp->err, io_err_unexpected_eof));

    FlateWriteError we = {7, io_err_short_write};
    Error werr = flate_write_error_as_error(&we, a);
    CHECK(str_eq(error_text(werr),
                 BURROW_S("flate: write error at offset 7: short write")));
    CHECK(str_eq(flate_write_error_error(&we, a), error_text(werr)));
    CHECK(errors_as(werr, TYPE_FLATE_WRITE_ERROR) != NULL);
    CHECK(errors_as(werr, TYPE_FLATE_READ_ERROR) == NULL);

    /* A clone outlives the arena the error was made in. */
    Arena ar2;
    arena_init(&ar2, NULL, 0);
    Error kept = error_retain(arena_allocator(&ar2), rerr);
    arena_free(&ar);
    CHECK(str_eq(error_text(kept),
                 BURROW_S("flate: read error at offset 42: unexpected EOF")));
    arena_free(&ar2);
}

/* The four readers in the library with ReadByte say so in their method sets,
 * and a reader with only Read does not. */
static void TestReadByteMethods(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br, BS("xy"));
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("xy"));
    BytesBuffer *bb = bytes_new_buffer(a, BS("xy"));
    BytesReader br2;
    bytes_reader_reset(&br2, BS("xy"));
    BufioReader *bu = bufio_new_reader(a, bytes_reader_as_io_reader(&br2));
    PlainReader plain = {bytes_reader_as_io_reader(&br)};
    IoReader rs[4] = {
        bytes_reader_as_io_reader(&br),
        strings_reader_as_io_reader(&sr),
        bytes_buffer_as_io_reader(bb),
        bufio_reader_as_io_reader(bu),
    };
    for (int i = 0; i < 4; i++) {
        const Method *m = burrow__io_read_byte_method(rs[i]);
        if (m == NULL) {
            testing_t_errorf_v(t, "reader %d has no ReadByte", i);
            continue;
        }
        Error e = BURROW_NO_ERROR;
        IoErrorArg ea = &e;
        Byte c = 0;
        void *args[1] = {(void *)&ea};
        void *rets[1] = {&c};
        CHECK(method_call(m, rs[i].data, args, rets));
        CHECK(BURROW_OK(e));
        CHECK_INT_EQ(c, 'x');
    }
    CHECK(burrow__io_read_byte_method(plain_of(&plain)) == NULL);
    CHECK(burrow__io_read_byte_method((IoReader){NULL, NULL}) == NULL);
    bufio_reader_free(bu);
    arena_free(&ar);
}

static void TestResetterOnlyForFlate(TestingT *t) {
    IoNopCloser nc = io_nop_closer((IoReader){NULL, NULL});
    IoReadCloser other = io_nop_closer_as_io_read_closer(&nc);
    CHECK(flate_reader_as_resetter(other).vt == NULL);
    flate_reader_free(other); /* leaves it alone */
    CHECK(flate_reader_as_resetter((IoReadCloser){NULL, NULL}).vt == NULL);
    flate_reader_free((IoReadCloser){NULL, NULL});
}

static void TestNoMemory(TestingT *t) {
    static unsigned char room[8192];
    Fixed fx;
    fixed_init(&fx, room, sizeof room);
    Alloc *a = fixed_allocator(&fx);
    BytesReader br;
    bytes_reader_reset(&br, BS("\x01\x00\x00\xff\xff"));
    CHECK(flate_new_reader(a, bytes_reader_as_io_reader(&br)).vt == NULL);
}

#define TESTS(X)                                                                       \
    X(TestIssue5915)                                                                   \
    X(TestIssue5962)                                                                   \
    X(TestIssue6255)                                                                   \
    X(TestInvalidEncoding)                                                             \
    X(TestInvalidBits)                                                                 \
    X(TestStreams)                                                                     \
    X(TestTruncatedStreams)                                                            \
    X(TestReaderEarlyEOF)                                                              \
    X(TestReset)                                                                       \
    X(TestReaderTruncated)                                                             \
    X(TestResetDict)                                                                   \
    X(TestReaderReusesReaderBuffer)                                                    \
    X(TestNlitOutOfRange)                                                              \
    X(TestDictDecoder)                                                                 \
    X(TestGoStreams)                                                                   \
    X(TestTruncatedGoStream)                                                           \
    X(TestCorruptInputError)                                                           \
    X(TestErrorTypes)                                                                  \
    X(TestReadByteMethods)                                                             \
    X(TestResetterOnlyForFlate)                                                        \
    X(TestNoMemory)

TESTING_MAIN(TESTS)
