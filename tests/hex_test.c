/* Derived from Go's src/encoding/hex/hex_test.go.
 * Go source: go1.27.1.
 *
 * Go compares errors with != and InvalidByteError is a byte, so two of them
 * for the same byte are equal. Here they are the same static error, and
 * errors_is is the same comparison. The tests at the end are new: the error
 * text for every byte, the panics Go's index checks give, and writers that
 * fail partway.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/hex.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"

#define BS(lit)                                                                        \
    slice_from((void *)(uintptr_t)(lit), (Int)sizeof(lit) - 1, (Int)sizeof(lit) - 1,   \
               TYPE_BYTE)

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

typedef struct EncDecTest {
    Str enc;
    Str dec;
} EncDecTest;

#define ED(enc, dec)                                                                   \
    {{(const Byte *)(enc), (Int)sizeof(enc) - 1},                                      \
     {(const Byte *)(dec), (Int)sizeof(dec) - 1}}

static const EncDecTest enc_dec_tests[] = {
    ED("", ""),
    ED("0001020304050607", "\x00\x01\x02\x03\x04\x05\x06\x07"),
    ED("08090a0b0c0d0e0f", "\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"),
    ED("f0f1f2f3f4f5f6f7", "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7"),
    ED("f8f9fafbfcfdfeff", "\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff"),
    ED("67", "g"),
    ED("e3a1", "\xe3\xa1"),
    /* Decoding only, since Encode always writes lowercase. */
    ED("F8F9FAFBFCFDFEFF", "\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff"),
};

#define N_ENC_DEC ((Int)(sizeof(enc_dec_tests) / sizeof(enc_dec_tests[0])) - 1)
#define N_DEC ((Int)(sizeof(enc_dec_tests) / sizeof(enc_dec_tests[0])))

static void TestEncode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_ENC_DEC; i++) {
        const EncDecTest *test = &enc_dec_tests[i];
        Int want = hex_encoded_len(test->dec.len);
        Slice dst = slice_make(a, TYPE_BYTE, want, want);
        Int n = hex_encode(dst, bytes_of(test->dec));
        if (n != dst.len)
            testing_t_errorf_v(t, "#%d: bad return value: got: %d want: %d", i, n,
                               dst.len);
        if (!str_eq(str_of(dst), test->enc))
            testing_t_errorf_v(t, "#%d: got: %q want: %q", i, str_of(dst), test->enc);
        dst = slice_from_str(a, BURROW_S("lead"));
        dst = hex_append_encode(a, dst, bytes_of(test->dec));
        Str lead = fmt_sprintf_v(a, "lead%s", test->enc);
        if (!str_eq(str_of(dst), lead))
            testing_t_errorf_v(t, "#%d: got: %q want: %q", i, str_of(dst), lead);
    }
    arena_free(&ar);
}

static void TestDecode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_DEC; i++) {
        const EncDecTest *test = &enc_dec_tests[i];
        Int want = hex_decoded_len(test->enc.len);
        Slice dst = slice_make(a, TYPE_BYTE, want, want);
        Error err;
        Int n = hex_decode(dst, bytes_of(test->enc), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: bad return value: got:%d want:%d", i, n,
                               dst.len);
        else if (!str_eq(str_of(dst), test->dec))
            testing_t_errorf_v(t, "#%d: got: %q want: %q", i, str_of(dst), test->dec);
        dst = slice_from_str(a, BURROW_S("lead"));
        dst = hex_append_decode(a, dst, bytes_of(test->enc), &err);
        Str lead = fmt_sprintf_v(a, "lead%s", test->dec);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: AppendDecode error: %s", i, error_text(err));
        else if (!str_eq(str_of(dst), lead))
            testing_t_errorf_v(t, "#%d: got: %q want: %q", i, str_of(dst), lead);
    }
    arena_free(&ar);
}

static void TestEncodeToString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_ENC_DEC; i++) {
        const EncDecTest *test = &enc_dec_tests[i];
        Str s = hex_encode_to_string(a, bytes_of(test->dec));
        if (!str_eq(s, test->enc))
            testing_t_errorf_v(t, "#%d got:%s want:%s", i, s, test->enc);
    }
    arena_free(&ar);
}

static void TestDecodeString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_ENC_DEC; i++) {
        const EncDecTest *test = &enc_dec_tests[i];
        Error err;
        Slice dst = hex_decode_string(a, test->enc, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: unexpected err value: %s", i, error_text(err));
            continue;
        }
        if (!str_eq(str_of(dst), test->dec))
            testing_t_errorf_v(t, "#%d: got: %q want: %q", i, str_of(dst), test->dec);
    }
    arena_free(&ar);
}

/* What the error should be: none, ErrLength, or InvalidByteError(bad). */
enum { E_NONE, E_LENGTH, E_BYTE };

typedef struct ErrTest {
    Str in;
    Str out;
    int kind;
    Byte bad;
} ErrTest;

#define ET(in, out, kind, bad)                                                         \
    {{(const Byte *)(in), (Int)sizeof(in) - 1},                                        \
     {(const Byte *)(out), (Int)sizeof(out) - 1},                                      \
     (kind),                                                                           \
     (bad)}

static const ErrTest err_tests[] = {
    ET("", "", E_NONE, 0),
    ET("0", "", E_LENGTH, 0),
    ET("zd4aa", "", E_BYTE, 'z'),
    ET("d4aaz", "\xd4\xaa", E_BYTE, 'z'),
    ET("30313", "01", E_LENGTH, 0),
    ET("0g", "", E_BYTE, 'g'),
    ET("00gg", "\x00", E_BYTE, 'g'),
    ET("0\x01", "", E_BYTE, '\x01'),
    ET("ffeed", "\xff\xee", E_LENGTH, 0),
};

#define N_ERR ((Int)(sizeof(err_tests) / sizeof(err_tests[0])))

static Error want_err(const ErrTest *tt, bool stream) {
    switch (tt->kind) {
    case E_LENGTH:
        /* The decoder is reading a stream, so it reports io.ErrUnexpectedEOF
         * instead of ErrLength. */
        return stream ? io_err_unexpected_eof : hex_err_length;
    case E_BYTE:
        return hex_invalid_byte_error_as_error(tt->bad);
    default:
        return BURROW_NO_ERROR;
    }
}

static void TestDecodeErr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_ERR; i++) {
        const ErrTest *tt = &err_tests[i];
        Slice out = slice_make(a, TYPE_BYTE, tt->in.len + 10, tt->in.len + 10);
        Error err;
        Int n = hex_decode(out, bytes_of(tt->in), &err);
        Error want = want_err(tt, false);
        Str got = str_from_bytes((const Byte *)out.p, n);
        if (!str_eq(got, tt->out) || !errors_is(err, want))
            testing_t_errorf_v(t, "Decode(%q) = %q, %s, want %q, %s", tt->in, got,
                               error_text(err), tt->out, error_text(want));
    }
    arena_free(&ar);
}

static void TestDecodeStringErr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_ERR; i++) {
        const ErrTest *tt = &err_tests[i];
        Error err;
        Slice out = hex_decode_string(a, tt->in, &err);
        Error want = want_err(tt, false);
        if (!str_eq(str_of(out), tt->out) || !errors_is(err, want))
            testing_t_errorf_v(t, "DecodeString(%q) = %q, %s, want %q, %s", tt->in,
                               str_of(out), error_text(err), tt->out, error_text(want));
    }
    arena_free(&ar);
}

/* A reader and a writer with nothing but Read and Write, so io_copy_buffer
 * cannot take a shortcut past the small buffer, as the Go test arranges with
 * struct{ io.Reader }. */
static Int only_read(void *self, Slice p, Error *err) {
    IoReader *r = (IoReader *)self;
    return r->vt->read(r->data, p, err);
}

static Int only_write(void *self, Slice p, Error *err) {
    IoWriter *w = (IoWriter *)self;
    return w->vt->write(w->data, p, err);
}

static const IoReaderVT only_reader_vt = {NULL, only_read};
static const IoWriterVT only_writer_vt = {NULL, only_write};

static void TestEncoderDecoder(TestingT *t) {
    static const Int multipliers[] = {1, 128, 192};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte small[7];
    Slice buf7 = slice_from(small, 7, 7, TYPE_BYTE);
    for (Int m = 0; m < 3; m++) {
        Int multiplier = multipliers[m];
        for (Int i = 0; i < N_ENC_DEC; i++) {
            const EncDecTest *test = &enc_dec_tests[i];
            Slice input = bytes_repeat(a, bytes_of(test->dec), multiplier);
            Str output = strings_repeat(a, test->enc, multiplier);

            BytesBuffer buf = BYTES_BUFFER(a);
            IoWriter enc = hex_new_encoder(a, bytes_buffer_as_io_writer(&buf));
            BytesReader br;
            bytes_reader_reset(&br, input);
            IoReader inner = bytes_reader_as_io_reader(&br);
            IoReader r = {&only_reader_vt, &inner};
            Error err;
            int64_t n = io_copy_buffer(enc, r, buf7, &err);
            if (n != input.len || BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "encoder.Write(%q*%d) = (%d, %s), want (%d, nil)",
                                   test->dec, multiplier, n, error_text(err),
                                   input.len);
                continue;
            }

            Str enc_dst = str_of(bytes_buffer_bytes(&buf));
            if (!str_eq(enc_dst, output)) {
                testing_t_errorf_v(t, "buf(%q*%d) = %s, want %s", test->dec, multiplier,
                                   enc_dst, output);
                continue;
            }

            IoReader dec = hex_new_decoder(a, bytes_buffer_as_io_reader(&buf));
            BytesBuffer dec_buf = BYTES_BUFFER(a);
            IoWriter inner_w = bytes_buffer_as_io_writer(&dec_buf);
            IoWriter w = {&only_writer_vt, &inner_w};
            io_copy_buffer(w, dec, buf7, &err);
            Slice got = bytes_buffer_bytes(&dec_buf);
            if (BURROW_FAILED(err) || got.len != input.len)
                testing_t_errorf_v(t, "decoder.Read(%q*%d) = (%d, %s), want (%d, nil)",
                                   test->enc, multiplier, got.len, error_text(err),
                                   input.len);
            if (!bytes_equal(got, input))
                testing_t_errorf_v(t, "decBuf(%q*%d) = %q, want %q", test->dec,
                                   multiplier, str_of(got), str_of(input));
        }
    }
    arena_free(&ar);
}

static void TestDecoderErr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_ERR; i++) {
        const ErrTest *tt = &err_tests[i];
        StringsReader sr;
        strings_reader_reset(&sr, tt->in);
        IoReader dec = hex_new_decoder(a, strings_reader_as_io_reader(&sr));
        /* io.ReadAll, which is a copy until the end. */
        BytesBuffer out = BYTES_BUFFER(a);
        Error err;
        io_copy(a, bytes_buffer_as_io_writer(&out), dec, &err);
        Error want = want_err(tt, true);
        Str got = str_of(bytes_buffer_bytes(&out));
        if (!str_eq(got, tt->out) || !errors_is(err, want))
            testing_t_errorf_v(t, "NewDecoder(%q) = %q, %s, want %q, %s", tt->in, got,
                               error_text(err), tt->out, error_text(want));
    }
    arena_free(&ar);
}

static const char expected_hex_dump[] =
    "00000000  1e 1f 20 21 22 23 24 25  26 27 28 29 2a 2b 2c 2d  |.. !\"#$%&'()*+,-|\n"
    "00000010  2e 2f 30 31 32 33 34 35  36 37 38 39 3a 3b 3c 3d  |./0123456789:;<=|\n"
    "00000020  3e 3f 40 41 42 43 44 45                           |>?@ABCDE|\n";

static Str expected_dump(void) {
    return str_from_bytes((const Byte *)expected_hex_dump,
                          (Int)sizeof(expected_hex_dump) - 1);
}

static void TestDumper(TestingT *t) {
    Byte in[40];
    for (Int i = 0; i < 40; i++)
        in[i] = (Byte)(i + 30);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int stride = 1; stride < 40; stride++) {
        BytesBuffer out = BYTES_BUFFER(a);
        IoWriteCloser dumper = hex_dumper(a, bytes_buffer_as_io_writer(&out));
        Int done = 0;
        while (done < 40) {
            Int todo = done + stride;
            if (todo > 40)
                todo = 40;
            dumper.vt->writer.write(
                dumper.data, slice_from(in + done, todo - done, todo - done, TYPE_BYTE),
                NULL);
            done = todo;
        }
        dumper.vt->closer.close(dumper.data);
        Str got = str_of(bytes_buffer_bytes(&out));
        if (!str_eq(got, expected_dump()))
            testing_t_errorf_v(t, "stride: %d failed. got:\n%s\nwant:\n%s", stride, got,
                               expected_dump());
    }
    arena_free(&ar);
}

static void TestDumper_doubleclose(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder out = STRINGS_BUILDER(a);
    IoWriteCloser dumper = hex_dumper(a, strings_builder_as_io_writer(&out));

    dumper.vt->writer.write(dumper.data, BS("gopher"), NULL);
    dumper.vt->closer.close(dumper.data);
    dumper.vt->closer.close(dumper.data);
    Error err;
    Int n = dumper.vt->writer.write(dumper.data, BS("gopher"), &err);
    dumper.vt->closer.close(dumper.data);

    Str expected = BURROW_S(
        "00000000  67 6f 70 68 65 72                                 |gopher|\n");
    if (!str_eq(strings_builder_string(&out), expected))
        testing_t_fatalf_v(t, "got:\n%q\nwant:\n%q", strings_builder_string(&out),
                           expected);
    CHECK(n == 0);
    CHECK(str_eq(error_text(err), BURROW_S("encoding/hex: dumper closed")));
    arena_free(&ar);
}

static void TestDumper_earlyclose(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder out = STRINGS_BUILDER(a);
    IoWriteCloser dumper = hex_dumper(a, strings_builder_as_io_writer(&out));

    dumper.vt->closer.close(dumper.data);
    dumper.vt->writer.write(dumper.data, BS("gopher"), NULL);

    if (strings_builder_string(&out).len != 0)
        testing_t_fatalf_v(t, "got:\n%q\nwant:\n%q", strings_builder_string(&out),
                           BURROW_S(""));
    arena_free(&ar);
}

static void TestDump(TestingT *t) {
    Byte in[40];
    for (Int i = 0; i < 40; i++)
        in[i] = (Byte)(i + 30);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str out = hex_dump(a, slice_from(in, 40, 40, TYPE_BYTE));
    if (!str_eq(out, expected_dump()))
        testing_t_errorf_v(t, "got:\n%s\nwant:\n%s", out, expected_dump());
    CHECK(hex_dump(a, slice_nil(TYPE_BYTE)).len == 0);
    arena_free(&ar);
}

/* ------------------------------------------------------------ not from Go */

/* Every byte's message, checked against the rule %#U follows: the code point,
 * then the character in quotes when strconv.IsPrint says it prints. */
static void TestInvalidByteErrorText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int b = 0; b < 256; b++) {
        Str want =
            strconv_is_print((Rune)b)
                ? fmt_sprintf_v(a, "encoding/hex: invalid byte: U+%04X '%c'", b, b)
                : fmt_sprintf_v(a, "encoding/hex: invalid byte: U+%04X", b);
        Str got = hex_invalid_byte_error_error((Byte)b);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "InvalidByteError(%d).Error() = %q, want %q", b, got,
                               want);
        Error err = hex_invalid_byte_error_as_error((Byte)b);
        if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "error_text(%d) = %q, want %q", b, error_text(err),
                               want);
        const HexInvalidByteError *e = errors_as(err, TYPE_HEX_INVALID_BYTE_ERROR);
        if (e == NULL || *e != (Byte)b)
            testing_t_errorf_v(t, "errors_as(%d) did not give the byte back", b);
    }
    CHECK(!errors_is(hex_invalid_byte_error_as_error('g'),
                     hex_invalid_byte_error_as_error('z')));
    arena_free(&ar);
}

static void encode_short(void *arg) {
    Byte dst[3];
    hex_encode(slice_from(dst, 3, 3, TYPE_BYTE), *(Slice *)arg);
}

static void decode_short(void *arg) {
    Byte dst[1];
    hex_decode(slice_from(dst, 1, 1, TYPE_BYTE), *(Slice *)arg, NULL);
}

static Any recovered(Func f) {
    volatile Any got = {NULL, NULL};
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        got = r;
    }
    BURROW_TRY_END;
    return got;
}

/* Go indexes past the end of dst and the runtime says where. */
static void TestShortDst(TestingT *t) {
    Slice src = BS("ab");
    Any v = recovered(BURROW_FN(Func, encode_short, &src));
    CHECK(str_eq(panic_text(v),
                 BURROW_S("runtime error: index out of range [3] with length 3")));
    Slice digits = BS("0a0b");
    v = recovered(BURROW_FN(Func, decode_short, &digits));
    CHECK(str_eq(panic_text(v),
                 BURROW_S("runtime error: index out of range [1] with length 1")));
}

/* A writer that takes limit bytes and then fails. */
typedef struct Limited {
    StringsBuilder *b;
    Int left;
} Limited;

BURROW_SENTINEL_ERROR(limited_full, "full");

static Int limited_write(void *self, Slice p, Error *err) {
    Limited *l = (Limited *)self;
    Int n = p.len < l->left ? p.len : l->left;
    strings_builder_write(l->b, slice_sub(p, 0, n), NULL);
    l->left -= n;
    *err = n < p.len ? limited_full : BURROW_NO_ERROR;
    return n;
}

static const IoWriterVT limited_vt = {NULL, limited_write};

static void TestWriteErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* The encoder counts the input bytes whose two digits both went out, and
     * stops for good after an error. */
    StringsBuilder sb = STRINGS_BUILDER(a);
    Limited l = {&sb, 5};
    IoWriter enc = hex_new_encoder(a, (IoWriter){&limited_vt, &l});
    Error err;
    Int n = enc.vt->write(enc.data, BS("abcd"), &err);
    CHECK(n == 2);
    CHECK(errors_is(err, limited_full));
    l.left = 100;
    n = enc.vt->write(enc.data, BS("abcd"), &err);
    CHECK(n == 0);
    CHECK(errors_is(err, limited_full));

    /* The dumper stops at whichever write failed: the offset, a byte, the end
     * of a line, or the padding in Close. Close has padding to write only when
     * a line was started. */
    static const struct {
        Int limit, wrote;
        bool write_fails, close_fails;
    } cases[] = {
        {0, 0, true, false},   {10, 0, true, false},
        {13, 1, true, true},   {10 + 46 + 4, 15, true, true},
        {79, 16, true, false}, {79 + 13, 17, false, true},
    };
    Byte line[17];
    for (Int i = 0; i < 17; i++)
        line[i] = (Byte)('a' + i);
    for (Int i = 0; i < 6; i++) {
        StringsBuilder b = STRINGS_BUILDER(a);
        Limited lim = {&b, cases[i].limit};
        IoWriteCloser d = hex_dumper(a, (IoWriter){&limited_vt, &lim});
        Int wrote =
            d.vt->writer.write(d.data, slice_from(line, 17, 17, TYPE_BYTE), &err);
        Error cerr = d.vt->closer.close(d.data);
        if (wrote != cases[i].wrote ||
            errors_is(err, limited_full) != cases[i].write_fails ||
            errors_is(cerr, limited_full) != cases[i].close_fails)
            testing_t_errorf_v(t, "limit %d: wrote %d, %s, close %s", cases[i].limit,
                               wrote, error_text(err), error_text(cerr));
    }
    arena_free(&ar);
}

/* A failed allocation gives nil interfaces and the out of memory error rather
 * than a crash. */
static void TestNoMemory(TestingT *t) {
    static unsigned char room[8];
    Fixed fx;
    fixed_init(&fx, room, sizeof room);
    Alloc *a = fixed_allocator(&fx);
    Byte big[64] = {0};
    Slice src = slice_from(big, 64, 64, TYPE_BYTE);
    CHECK(hex_new_encoder(a, (IoWriter){NULL, NULL}).vt == NULL);
    CHECK(hex_new_decoder(a, (IoReader){NULL, NULL}).vt == NULL);
    CHECK(hex_dumper(a, (IoWriter){NULL, NULL}).vt == NULL);
    CHECK(hex_encode_to_string(a, src).len == 0);
    CHECK(hex_dump(a, src).len == 0);
    Error err;
    hex_decode_string(a, BURROW_S("00112233445566778899aabbccddeeff"), &err);
    CHECK(errors_is(err, burrow_err_out_of_memory));
    hex_append_decode(a, slice_nil(TYPE_BYTE), src, &err);
    CHECK(errors_is(err, burrow_err_out_of_memory));
    CHECK(hex_append_encode(a, (Slice){0}, src).len == 0);
    CHECK(hex_encode_to_string(a, slice_nil(TYPE_BYTE)).len == 0);
}

/* ------------------------------------------------------------ benchmarks */

static const Int bench_sizes[] = {256, 1024, 4096, 16384};

static void BenchmarkEncode(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int size = bench_sizes[1];
    Slice src = bytes_repeat(a, BS("\x02\x03\x05\x07\x09\x0b\x0d\x11"), size / 8);
    Slice sink = slice_make(a, TYPE_BYTE, 2 * size, 2 * size);
    testing_b_set_bytes(b, size);
    for (Int i = 0; i < testing_b_n(b); i++)
        hex_encode(sink, src);
    arena_free(&ar);
}

static void BenchmarkDecode(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int size = bench_sizes[1];
    Slice src = bytes_repeat(a, BS("2b744faa"), size / 8);
    Slice sink = slice_make(a, TYPE_BYTE, size / 2, size / 2);
    testing_b_set_bytes(b, size);
    for (Int i = 0; i < testing_b_n(b); i++)
        hex_decode(sink, src, NULL);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEncode)                                                                      \
    X(TestDecode)                                                                      \
    X(TestEncodeToString)                                                              \
    X(TestDecodeString)                                                                \
    X(TestDecodeErr)                                                                   \
    X(TestDecodeStringErr)                                                             \
    X(TestEncoderDecoder)                                                              \
    X(TestDecoderErr)                                                                  \
    X(TestDumper)                                                                      \
    X(TestDumper_doubleclose)                                                          \
    X(TestDumper_earlyclose)                                                           \
    X(TestDump)                                                                        \
    X(TestInvalidByteErrorText)                                                        \
    X(TestShortDst)                                                                    \
    X(TestWriteErrors)                                                                 \
    X(TestNoMemory)                                                                    \
    X(BenchmarkEncode)                                                                 \
    X(BenchmarkDecode)

TESTING_MAIN(TESTS)
