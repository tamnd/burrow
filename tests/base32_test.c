/* Derived from Go's src/encoding/base32/base32_test.go.
 * Go source: go1.27.1.
 *
 * Go's TestDecode calls the unexported decode to look at its end flag, which
 * is not reachable from here, so that check is left out and the rest of the
 * test kept. The two tests that feed the decoder through an io.Pipe from a
 * goroutine use a reader that hands out the same chunks one Read at a time,
 * which is what the pipe does. The tests at the end are new: the panics
 * NewEncoding, WithPadding and a short dst give, a writer that fails, and
 * running out of memory.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/base32.h"
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

typedef struct TestPair {
    Str decoded;
    Str encoded;
} TestPair;

#define TP(dec, enc)                                                                   \
    {{(const Byte *)(dec), (Int)sizeof(dec) - 1},                                      \
     {(const Byte *)(enc), (Int)sizeof(enc) - 1}}

static const TestPair pairs[] = {
    /* RFC 4648 examples */
    TP("", ""),
    TP("f", "MY======"),
    TP("fo", "MZXQ===="),
    TP("foo", "MZXW6==="),
    TP("foob", "MZXW6YQ="),
    TP("fooba", "MZXW6YTB"),
    TP("foobar", "MZXW6YTBOI======"),

    /* Wikipedia examples, converted to base32 */
    TP("sure.", "ON2XEZJO"),
    TP("sure", "ON2XEZI="),
    TP("sur", "ON2XE==="),
    TP("su", "ON2Q===="),
    TP("leasure.", "NRSWC43VOJSS4==="),
    TP("easure.", "MVQXG5LSMUXA===="),
    TP("asure.", "MFZXK4TFFY======"),
    TP("sure.", "ON2XEZJO"),
};

#define N_PAIRS ((Int)(sizeof(pairs) / sizeof(pairs[0])))

static const TestPair bigtest =
    TP("Twas brillig, and the slithy toves",
       "KR3WC4ZAMJZGS3DMNFTSYIDBNZSCA5DIMUQHG3DJORUHSIDUN53GK4Y=");

static bool is_eof(Error e) {
    return errors_is(e, io_eof);
}

static Base32Encoding raw_std(void) {
    return base32_encoding_with_padding(base32_std_encoding, BASE32_NO_PADDING);
}

static void TestEncode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        Str got = base32_encoding_encode_to_string(base32_std_encoding, a,
                                                   bytes_of(p->decoded));
        if (!str_eq(got, p->encoded))
            testing_t_errorf_v(t, "Encode(%q) = %q, want %q", p->decoded, got,
                               p->encoded);
        Slice dst = base32_encoding_append_encode(base32_std_encoding, a,
                                                  slice_from_str(a, BURROW_S("lead")),
                                                  bytes_of(p->decoded));
        Str lead = fmt_sprintf_v(a, "lead%s", p->encoded);
        if (!str_eq(str_of(dst), lead))
            testing_t_errorf_v(t, "AppendEncode(\"lead\", %q) = %q, want %q",
                               p->decoded, str_of(dst), lead);
    }
    arena_free(&ar);
}

static void TestEncoder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        StringsBuilder bb = STRINGS_BUILDER(a);
        IoWriteCloser encoder = base32_new_encoder(a, base32_std_encoding,
                                                   strings_builder_as_io_writer(&bb));
        encoder.vt->writer.write(encoder.data, bytes_of(p->decoded), NULL);
        encoder.vt->closer.close(encoder.data);
        Str got = strings_builder_string(&bb);
        if (!str_eq(got, p->encoded))
            testing_t_errorf_v(t, "Encode(%q) = %q, want %q", p->decoded, got,
                               p->encoded);
    }
    arena_free(&ar);
}

static void TestEncoderBuffering(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice input = bytes_of(bigtest.decoded);
    for (Int bs = 1; bs <= 12; bs++) {
        StringsBuilder bb = STRINGS_BUILDER(a);
        IoWriteCloser encoder = base32_new_encoder(a, base32_std_encoding,
                                                   strings_builder_as_io_writer(&bb));
        for (Int pos = 0; pos < input.len; pos += bs) {
            Int end = pos + bs;
            if (end > input.len)
                end = input.len;
            Slice chunk = slice_sub(input, pos, end);
            Error err;
            Int n = encoder.vt->writer.write(encoder.data, chunk, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "Write(%q) gave error %s, want nil",
                                   str_of(chunk), error_text(err));
            if (n != end - pos)
                testing_t_errorf_v(t, "Write(%q) gave length %d, want %d",
                                   str_of(chunk), n, end - pos);
        }
        Error err = encoder.vt->closer.close(encoder.data);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Close gave error %s, want nil", error_text(err));
        Str got = strings_builder_string(&bb);
        if (!str_eq(got, bigtest.encoded))
            testing_t_errorf_v(t, "Encoding/%d of %q = %q, want %q", bs,
                               bigtest.decoded, got, bigtest.encoded);
    }
    arena_free(&ar);
}

static void decoder_buffering(TestingT *t, bool padded) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Base32Encoding raw = raw_std();
    const Base32Encoding *enc = padded ? base32_std_encoding : &raw;
    for (Int bs = 0; bs <= 12; bs++) {
        for (Int i = 0; i < N_PAIRS; i++) {
            const TestPair *s = &pairs[i];
            Str encoded =
                padded ? s->encoded : strings_trim_right(s->encoded, BURROW_S("="));
            StringsReader sr;
            strings_reader_reset(&sr, encoded);
            IoReader decoder =
                base32_new_decoder(a, enc, strings_reader_as_io_reader(&sr));
            Int size = s->decoded.len + bs;
            Slice buf = slice_make(a, TYPE_BYTE, size, size);
            Error err;
            Int n = decoder.vt->read(decoder.data, buf, &err);
            if (BURROW_FAILED(err) && !is_eof(err))
                testing_t_errorf_v(t,
                                   "Read from %q at pos %d = %d, unexpected error %s",
                                   encoded, s->decoded.len, n, error_text(err));
            Str got = str_of(slice_sub(buf, 0, n));
            if (!str_eq(got, s->decoded))
                testing_t_errorf_v(t, "Decoding/%d of %q = %q, want %q", bs, encoded,
                                   got, s->decoded);
        }
    }
    arena_free(&ar);
}

static void TestDecoderBufferingWithPadding(TestingT *t) {
    decoder_buffering(t, true);
}

static void TestDecoderBufferingWithoutPadding(TestingT *t) {
    decoder_buffering(t, false);
}

static void TestDecode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        Int dlen = base32_encoding_decoded_len(base32_std_encoding, p->encoded.len);
        Slice dbuf = slice_make(a, TYPE_BYTE, dlen, dlen);
        Error err;
        Int count = base32_encoding_decode(base32_std_encoding, dbuf,
                                           bytes_of(p->encoded), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Decode(%q) = error %s, want nil", p->encoded,
                               error_text(err));
        if (count != p->decoded.len)
            testing_t_errorf_v(t, "Decode(%q) = length %d, want %d", p->encoded, count,
                               p->decoded.len);
        if (!str_eq(str_of(slice_sub(dbuf, 0, count)), p->decoded))
            testing_t_errorf_v(t, "Decode(%q) = %q, want %q", p->encoded,
                               str_of(slice_sub(dbuf, 0, count)), p->decoded);

        dbuf = base32_encoding_decode_string(base32_std_encoding, a, p->encoded, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "DecodeString(%q) = error %s, want nil", p->encoded,
                               error_text(err));
        if (!str_eq(str_of(dbuf), p->decoded))
            testing_t_errorf_v(t, "DecodeString(%q) = %q, want %q", p->encoded,
                               str_of(dbuf), p->decoded);

        Slice dst = base32_encoding_append_decode(base32_std_encoding, a,
                                                  slice_from_str(a, BURROW_S("lead")),
                                                  bytes_of(p->encoded), &err);
        Str lead = fmt_sprintf_v(a, "lead%s", p->decoded);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "AppendDecode(%q) = error %s, want nil", p->encoded,
                               error_text(err));
        if (!str_eq(str_of(dst), lead))
            testing_t_errorf_v(t, "AppendDecode(\"lead\", %q) = %q, want %q",
                               p->encoded, str_of(dst), lead);

        Slice dst2 = base32_encoding_append_decode(
            base32_std_encoding, a, slice_sub3(dst, 0, 0, p->decoded.len),
            bytes_of(p->encoded), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "AppendDecode(%q) = error %s, want nil", p->encoded,
                               error_text(err));
        if (!str_eq(str_of(dst2), p->decoded))
            testing_t_errorf_v(t, "AppendDecode(\"\", %q) = %q, want %q", p->encoded,
                               str_of(dst2), p->decoded);
        if (dst.len > 0 && dst2.len > 0 && dst.p != dst2.p)
            testing_t_errorf_v(t, "unexpected capacity growth: got %d, want %d",
                               dst2.cap, dst.cap);
    }
    arena_free(&ar);
}

static void TestDecoder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        StringsReader sr;
        strings_reader_reset(&sr, p->encoded);
        IoReader decoder = base32_new_decoder(a, base32_std_encoding,
                                              strings_reader_as_io_reader(&sr));
        Int dlen = base32_encoding_decoded_len(base32_std_encoding, p->encoded.len);
        Slice dbuf = slice_make(a, TYPE_BYTE, dlen, dlen);
        Error err;
        Int count = decoder.vt->read(decoder.data, dbuf, &err);
        if (BURROW_FAILED(err) && !is_eof(err))
            testing_t_fatalf_v(t, "Read failed %s", error_text(err));
        if (count != p->decoded.len)
            testing_t_errorf_v(t, "Read from %q = length %d, want %d", p->encoded,
                               count, p->decoded.len);
        if (!str_eq(str_of(slice_sub(dbuf, 0, count)), p->decoded))
            testing_t_errorf_v(t, "Decoding of %q = %q, want %q", p->encoded,
                               str_of(slice_sub(dbuf, 0, count)), p->decoded);
        if (!is_eof(err))
            decoder.vt->read(decoder.data, dbuf, &err);
        if (!is_eof(err))
            testing_t_errorf_v(t, "Read from %q = %s, want EOF", p->encoded,
                               error_text(err));
    }
    arena_free(&ar);
}

/* Go's badReader: it fills p with as much data as it has, up to limit when
 * limit is set, and returns the next error from errs, or io.EOF once they
 * run out. A zero Error in errs is Go's nil. */
typedef struct BadReader {
    Str data;
    const Error *errs;
    Int nerrs;
    Int called;
    Int limit;
} BadReader;

static Int bad_read(void *self, Slice p, Error *err) {
    BadReader *b = (BadReader *)self;
    Int lim = p.len;
    if (b->limit != 0 && b->limit < lim)
        lim = b->limit;
    if (b->data.len < lim)
        lim = b->data.len;
    if (lim > 0)
        memcpy(p.p, b->data.p, (size_t)lim);
    b->data.p += lim;
    b->data.len -= lim;
    Error e = io_eof;
    if (b->called < b->nerrs)
        e = b->errs[b->called];
    b->called++;
    *err = e;
    return lim;
}

static const IoReaderVT bad_reader_vt = {NULL, bad_read};

BURROW_SENTINEL_ERROR(bad_err, "bad reader error");

typedef struct Issue20044 {
    const char *data;
    Error errs[2];
    Int nerrs;
    Int limit;
    const char *res;
    Error err;
    Int dbuflen;
} Issue20044;

static void TestIssue20044(TestingT *t) {
    const Error nil = BURROW_NO_ERROR;
    const Issue20044 cases[] = {
        /* Valid input data with an error is decoded and the error passed on. */
        {"MY======", {bad_err}, 1, 0, "f", bad_err, 0},
        /* A read error with nothing but newlines is passed on. */
        {"\n\n\n\n\n\n\n\n", {bad_err, nil}, 2, 0, "", bad_err, 0},
        /* Eight newlines first, then valid data and an error. */
        {"\n\n\n\n\n\n\n\nMY======", {nil, bad_err}, 2, 0, "f", bad_err, 8},
        /* Input too short, with an error: the reader's error wins. */
        {"MY=====", {bad_err}, 1, 0, "", bad_err, 0},
        /* Input too short and no error: io.ErrUnexpectedEOF. */
        {"MY=====", {nil}, 1, 0, "", io_err_unexpected_eof, 0},
        /* Bad input with an error: the reader's error wins over the decoder's. */
        {"Ma======", {bad_err}, 1, 0, "", bad_err, 0},
        /* Valid data and io.EOF. */
        {"MZXW6YTB", {io_eof}, 1, 0, "fooba", io_eof, 0},
        /* Errors across many Reads of one byte. */
        {"NRSWC43VOJSS4===", {nil, bad_err}, 2, 0, "leasure.", bad_err, 1},
        {"NRSWC43VOJSS4===", {nil, io_eof}, 2, 0, "leasure.", io_eof, 1},
        /* More than eight bytes a Read. */
        {"NRSWC43VOJSS4===", {io_eof}, 1, 0, "leasure.", io_eof, 11},
        {"NRSWC43VOJSS4===", {bad_err}, 1, 0, "leasure.", bad_err, 11},
        /* Reads of 11 and then 7 bytes. */
        {"NRSWC43VOJSS4===", {nil, bad_err}, 2, 11, "leasure.", bad_err, 0},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Issue20044 *tc = &cases[i];
        Str input = str_from_cstr(tc->data);
        BadReader br = {input, tc->errs, tc->nerrs, 0, tc->limit};
        IoReader decoder =
            base32_new_decoder(a, base32_std_encoding, (IoReader){&bad_reader_vt, &br});
        Int dbuflen = tc->dbuflen > 0
                          ? tc->dbuflen
                          : base32_encoding_decoded_len(base32_std_encoding, input.len);
        Slice dbuf = slice_make(a, TYPE_BYTE, dbuflen, dbuflen);
        Error err = BURROW_NO_ERROR;
        StringsBuilder res = STRINGS_BUILDER(a);
        while (BURROW_OK(err)) {
            Int n = decoder.vt->read(decoder.data, dbuf, &err);
            if (n > 0)
                strings_builder_write(&res, slice_sub(dbuf, 0, n), NULL);
        }
        Str got = strings_builder_string(&res);
        if (!str_eq(got, str_from_cstr(tc->res)))
            testing_t_errorf_v(t, "Decoding of %q = %q, want %q", input, got,
                               str_from_cstr(tc->res));
        if (!errors_is(err, tc->err))
            testing_t_errorf_v(t, "Decoding of %q err = %s, expected %s", input,
                               error_text(err), error_text(tc->err));
    }
    arena_free(&ar);
}

static void TestDecoderError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Error read_errs[] = {io_eof, BURROW_NO_ERROR};
    for (int k = 0; k < 2; k++) {
        Str input = BURROW_S("MZXW6YTb");
        Int dlen = base32_encoding_decoded_len(base32_std_encoding, input.len);
        Slice dbuf = slice_make(a, TYPE_BYTE, dlen, dlen);
        BadReader br = {input, &read_errs[k], 1, 0, 0};
        IoReader decoder =
            base32_new_decoder(a, base32_std_encoding, (IoReader){&bad_reader_vt, &br});
        Error err;
        Int n = decoder.vt->read(decoder.data, dbuf, &err);
        if (n != 0)
            testing_t_errorf_v(t, "Read after EOF, n = %d, expected %d", n, (Int)0);
        if (errors_as(err, TYPE_BASE32_CORRUPT_INPUT_ERROR) == NULL)
            testing_t_errorf_v(t, "Corrupt input error expected.  Found %s",
                               error_text(err));
    }
    arena_free(&ar);
}

static void TestReaderEOF(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int k = 0; k < 2; k++) {
        const Error errs[] = {BURROW_NO_ERROR, k == 0 ? io_eof : BURROW_NO_ERROR};
        Str input = BURROW_S("MZXW6YTB");
        BadReader br = {input, errs, 2, 0, 0};
        IoReader decoder =
            base32_new_decoder(a, base32_std_encoding, (IoReader){&bad_reader_vt, &br});
        Int dlen = base32_encoding_decoded_len(base32_std_encoding, input.len);
        Slice dbuf = slice_make(a, TYPE_BYTE, dlen, dlen);
        Error err;
        decoder.vt->read(decoder.data, dbuf, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Decoding of %q err = %s, expected nil", input,
                               error_text(err));
        for (int j = 0; j < 2; j++) {
            Int n = decoder.vt->read(decoder.data, dbuf, &err);
            if (n != 0)
                testing_t_errorf_v(t, "Read after EOF, n = %d, expected %d", n, (Int)0);
            if (!is_eof(err))
                testing_t_errorf_v(t, "Read after EOF, err = %s, expected EOF",
                                   error_text(err));
        }
    }
    arena_free(&ar);
}

static void TestDecoderBuffering(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int bs = 1; bs <= 12; bs++) {
        StringsReader sr;
        strings_reader_reset(&sr, bigtest.encoded);
        IoReader decoder = base32_new_decoder(a, base32_std_encoding,
                                              strings_reader_as_io_reader(&sr));
        Int size = bigtest.decoded.len + 12;
        Slice buf = slice_make(a, TYPE_BYTE, size, size);
        Int total, n = 0;
        Error err = BURROW_NO_ERROR;
        for (total = 0; total < bigtest.decoded.len && BURROW_OK(err);) {
            n = decoder.vt->read(decoder.data, slice_sub(buf, total, total + bs), &err);
            total += n;
        }
        if (BURROW_FAILED(err) && !is_eof(err))
            testing_t_errorf_v(t, "Read from %q at pos %d = %d, unexpected error %s",
                               bigtest.encoded, total, n, error_text(err));
        Str got = str_of(slice_sub(buf, 0, total));
        if (!str_eq(got, bigtest.decoded))
            testing_t_errorf_v(t, "Decoding/%d of %q = %q, want %q", bs,
                               bigtest.encoded, got, bigtest.decoded);
    }
    arena_free(&ar);
}

typedef struct CorruptTest {
    Str input;
    Int offset; /* -1 means no corruption */
} CorruptTest;

#define CT(in, off) {{(const Byte *)(in), (Int)sizeof(in) - 1}, off}

static const CorruptTest corrupt_tests[] = {
    CT("", -1),         CT("!!!!", 0),      CT("x===", 0),      CT("AA=A====", 2),
    CT("AAA=AAAA", 3),  CT("MMMMMMMMM", 8), CT("MMMMMM", 0),    CT("A=", 1),
    CT("AA=", 3),       CT("AA==", 4),      CT("AA===", 5),     CT("AAAA=", 5),
    CT("AAAA==", 6),    CT("AAAAA=", 6),    CT("AAAAA==", 7),   CT("A=======", 1),
    CT("AA======", -1), CT("AAA=====", 3),  CT("AAAA====", -1), CT("AAAAA===", -1),
    CT("AAAAAA==", 6),  CT("AAAAAAA=", -1), CT("AAAAAAAA", -1),
};

static void TestDecodeCorrupt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof corrupt_tests / sizeof corrupt_tests[0]; i++) {
        const CorruptTest *tc = &corrupt_tests[i];
        Int dlen = base32_encoding_decoded_len(base32_std_encoding, tc->input.len);
        Slice dbuf = slice_make(a, TYPE_BYTE, dlen, dlen);
        Error err;
        base32_encoding_decode(base32_std_encoding, dbuf, bytes_of(tc->input), &err);
        if (tc->offset == -1) {
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "Decoder wrongly detected corruption in %q",
                                   tc->input);
            continue;
        }
        const Base32CorruptInputError *c = (const Base32CorruptInputError *)errors_as(
            err, TYPE_BASE32_CORRUPT_INPUT_ERROR);
        if (c == NULL)
            testing_t_errorf_v(t, "Decoder failed to detect corruption in %q",
                               tc->input);
        else if (*c != tc->offset)
            testing_t_errorf_v(t, "Corruption in %q at offset %d, want %d", tc->input,
                               (Int)*c, tc->offset);
    }
    arena_free(&ar);
}

/* io.ReadAll, which is a copy until the end. */
static Slice read_all(Alloc *a, IoReader r, Error *err) {
    BytesBuffer out = BYTES_BUFFER(a);
    io_copy(a, bytes_buffer_as_io_writer(&out), r, err);
    return bytes_buffer_bytes(&out);
}

static void TestBig(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = 3 * 1000 + 1;
    static const char alpha[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    Slice raw = slice_make(a, TYPE_BYTE, n, n);
    for (Int i = 0; i < n; i++)
        ((Byte *)raw.p)[i] = (Byte)alpha[i % (Int)(sizeof alpha - 1)];
    BytesBuffer encoded = BYTES_BUFFER(a);
    IoWriteCloser w =
        base32_new_encoder(a, base32_std_encoding, bytes_buffer_as_io_writer(&encoded));
    Error err;
    Int nn = w.vt->writer.write(w.data, raw, &err);
    if (nn != n || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Encoder.Write(raw) = %d, %s want %d, nil", nn,
                           error_text(err), n);
    err = w.vt->closer.close(w.data);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Encoder.Close() = %s want nil", error_text(err));
    Slice decoded = read_all(
        a,
        base32_new_decoder(a, base32_std_encoding, bytes_buffer_as_io_reader(&encoded)),
        &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "io.ReadAll(NewDecoder(...)): %s", error_text(err));
    if (!bytes_equal(raw, decoded)) {
        Int i;
        for (i = 0; i < decoded.len && i < raw.len; i++)
            if (((Byte *)decoded.p)[i] != ((Byte *)raw.p)[i])
                break;
        testing_t_errorf_v(t, "Decode(Encode(%d-byte string)) failed at offset %d", n,
                           i);
    }
    arena_free(&ar);
}

static void string_encoding(TestingT *t, Alloc *a, Str expected,
                            const char *const *examples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        Str e = str_from_cstr(examples[i]);
        Error err;
        Slice buf = base32_encoding_decode_string(base32_std_encoding, a, e, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Decode(%q) failed: %s", e, error_text(err));
            continue;
        }
        if (!str_eq(str_of(buf), expected))
            testing_t_errorf_v(t, "Decode(%q) = %q, want %q", e, str_of(buf), expected);
    }
}

static void TestNewLineCharacters(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Each of these should decode to the string "sure", without errors. */
    static const char *const examples[] = {
        "ON2XEZI=",     "ON2XEZI=\r",   "ON2XEZI=\n", "ON2XEZI=\r\n", "ON2XEZ\r\nI=",
        "ON2X\rEZ\nI=", "ON2X\nEZ\rI=", "ON2XEZ\nI=", "ON2XEZI\n=",
    };
    string_encoding(t, a, BURROW_S("sure"), examples,
                    sizeof examples / sizeof examples[0]);
    /* Each of these should decode to the string "foobar", without errors. */
    static const char *const examples2[] = {"MZXW6YTBOI======", "MZXW6YTBOI=\r\n====="};
    string_encoding(t, a, BURROW_S("foobar"), examples2,
                    sizeof examples2 / sizeof examples2[0]);
    arena_free(&ar);
}

static void TestDecoderIssue4779(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str encoded =
        BURROW_S("JRXXEZLNEBUXA43VNUQGI33MN5ZCA43JOQQGC3LFOQWCAY3PNZZWKY3UMV2HK4\n"
                 "RAMFSGS4DJONUWG2LOM4QGK3DJOQWCA43FMQQGI3YKMVUXK43NN5SCA5DFNVYG64RANFX"
                 "GG2LENFSH\n"
                 "K3TUEB2XIIDMMFRG64TFEBSXIIDEN5WG64TFEBWWCZ3OMEQGC3DJOF2WCLRAKV2CAZLON"
                 "FWQUYLEEB\n"
                 "WWS3TJNUQHMZLONFQW2LBAOF2WS4ZANZXXG5DSOVSCAZLYMVZGG2LUMF2GS33OEB2WY3D"
                 "BNVRW6IDM\n"
                 "MFRG64TJOMQG42LTNEQHK5AKMFWGS4LVNFYCAZLYEBSWCIDDN5WW233EN4QGG33OONSXC"
                 "5LBOQXCAR\n"
                 "DVNFZSAYLVORSSA2LSOVZGKIDEN5WG64RANFXAU4TFOBZGK2DFNZSGK4TJOQQGS3RAOZX"
                 "WY5LQORQX\n"
                 "IZJAOZSWY2LUEBSXG43FEBRWS3DMOVWSAZDPNRXXEZJAMV2SAZTVM5UWC5BANZ2WY3DBB"
                 "JYGC4TJMF\n"
                 "2HK4ROEBCXQY3FOB2GK5LSEBZWS3TUEBXWGY3BMVRWC5BAMN2XA2LEMF2GC5BANZXW4ID"
                 "QOJXWSZDF\n"
                 "NZ2CYIDTOVXHIIDJNYFGG5LMOBQSA4LVNEQG6ZTGNFRWSYJAMRSXGZLSOVXHIIDNN5WGY"
                 "2LUEBQW42\n"
                 "LNEBUWIIDFON2CA3DBMJXXE5LNFY==\n"
                 "====");
    Str encoded_short = strings_replace_all(a, encoded, BURROW_S("\n"), BURROW_S(""));

    StringsReader sr;
    strings_reader_reset(&sr, encoded);
    Error err;
    Slice res1 = read_all(
        a, base32_new_decoder(a, base32_std_encoding, strings_reader_as_io_reader(&sr)),
        &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadAll failed: %s", error_text(err));

    StringsReader sr2;
    strings_reader_reset(&sr2, encoded_short);
    Slice res2 = read_all(
        a,
        base32_new_decoder(a, base32_std_encoding, strings_reader_as_io_reader(&sr2)),
        &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadAll failed: %s", error_text(err));

    if (!bytes_equal(res1, res2))
        testing_t_errorf_v(t, "Decoded results not equal");
    arena_free(&ar);
}

static void TestWithCustomPadding(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Base32Encoding at = base32_encoding_with_padding(base32_std_encoding, '@');
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *tc = &pairs[i];
        Str def = base32_encoding_encode_to_string(base32_std_encoding, a,
                                                   bytes_of(tc->decoded));
        Str custom = base32_encoding_encode_to_string(&at, a, bytes_of(tc->decoded));
        Str expected = strings_replace_all(a, def, BURROW_S("="), BURROW_S("@"));
        if (!str_eq(expected, custom))
            testing_t_errorf_v(t, "Expected custom %s, got %s", expected, custom);
        if (!str_eq(tc->encoded, def))
            testing_t_errorf_v(t, "Expected %s, got %s", tc->encoded, def);
    }
    arena_free(&ar);
}

static void TestWithoutPadding(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Base32Encoding raw = raw_std();
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *tc = &pairs[i];
        Str def = base32_encoding_encode_to_string(base32_std_encoding, a,
                                                   bytes_of(tc->decoded));
        Str custom = base32_encoding_encode_to_string(&raw, a, bytes_of(tc->decoded));
        Str expected = strings_trim_right(def, BURROW_S("="));
        if (!str_eq(expected, custom))
            testing_t_errorf_v(t, "Expected custom %s, got %s", expected, custom);
        if (!str_eq(tc->encoded, def))
            testing_t_errorf_v(t, "Expected %s, got %s", tc->encoded, def);
    }
    arena_free(&ar);
}

static void TestDecodeWithPadding(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Base32Encoding encodings[] = {
        *base32_std_encoding,
        base32_encoding_with_padding(base32_std_encoding, '-'),
        raw_std(),
    };
    for (int k = 0; k < 3; k++) {
        const Base32Encoding *enc = &encodings[k];
        for (Int i = 0; i < N_PAIRS; i++) {
            Str input = pairs[i].decoded;
            Str encoded = base32_encoding_encode_to_string(enc, a, bytes_of(input));
            Error err;
            Slice decoded = base32_encoding_decode_string(enc, a, encoded, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "DecodeString Error for encoding %d (%q): %s",
                                   (Int)k, input, error_text(err));
            if (!str_eq(input, str_of(decoded)))
                testing_t_errorf_v(t,
                                   "Unexpected result for encoding %d: got %q; want %q",
                                   (Int)k, str_of(decoded), input);
        }
    }
    arena_free(&ar);
}

static void TestDecodeWithWrongPadding(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str encoded =
        base32_encoding_encode_to_string(base32_std_encoding, a, BS("foobar"));
    Base32Encoding dash = base32_encoding_with_padding(base32_std_encoding, '-');
    Error err;
    base32_encoding_decode_string(&dash, a, encoded, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error");
    Base32Encoding raw = raw_std();
    base32_encoding_decode_string(&raw, a, encoded, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error");
    arena_free(&ar);
}

/* What an io.Pipe gives a reader when the other end writes these chunks and
 * closes: each Read gets what is left of the current chunk, as much as fits,
 * and then io.EOF. */
typedef struct ChunkReader {
    const char *const *chunks;
    Int n;
    Int at;
    Int off;
} ChunkReader;

static Int chunk_read(void *self, Slice p, Error *err) {
    ChunkReader *c = (ChunkReader *)self;
    if (c->at == c->n) {
        *err = io_eof;
        return 0;
    }
    const char *s = c->chunks[c->at];
    Int left = (Int)strlen(s) - c->off;
    Int k = p.len < left ? p.len : left;
    memcpy(p.p, s + c->off, (size_t)k);
    c->off += k;
    if (c->off == (Int)strlen(s)) {
        c->at++;
        c->off = 0;
    }
    *err = BURROW_NO_ERROR;
    return k;
}

static const IoReaderVT chunk_reader_vt = {NULL, chunk_read};

#define MAX_CHUNKS 6

typedef struct ChunkCase {
    const char *chunks[MAX_CHUNKS];
} ChunkCase;

static Int count_chunks(const ChunkCase *c) {
    Int n = 0;
    while (n < MAX_CHUNKS && c->chunks[n] != NULL)
        n++;
    return n;
}

static void same_error(TestingT *t, Alloc *a, const char *prefix,
                       const ChunkCase *cases, size_t ncases, Error expected) {
    for (size_t i = 0; i < ncases; i++) {
        ChunkReader cr = {cases[i].chunks, count_chunks(&cases[i]), 0, 0};
        IoReader decoder = base32_new_decoder(a, base32_std_encoding,
                                              (IoReader){&chunk_reader_vt, &cr});
        Error err;
        read_all(a, decoder, &err);
        bool ok = BURROW_OK(expected) ? BURROW_OK(err) : errors_is(err, expected);
        if (!ok)
            testing_t_errorf_v(t, "Expected %s, got %s; case %s #%d",
                               error_text(expected), error_text(err),
                               str_from_cstr(prefix), (Int)i);
    }
}

static void TestBufferedDecodingSameError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* NBSWY3DPO5XXE3DE == helloworld, with "ZZ" as extra input */
    static const ChunkCase zz[] = {
        {{"NBSW", "Y3DP", "O5XX", "E3DE", "ZZ"}},
        {{"NBSWY3DPO5XXE3DE", "ZZ"}},
        {{"NBSWY3DPO5XXE3DEZZ"}},
        {{"NBS", "WY3", "DPO", "5XX", "E3D", "EZZ"}},
        {{"NBSWY3DPO5XXE3", "DEZZ"}},
    };
    same_error(t, a, "helloworld", zz, sizeof zz / sizeof zz[0], io_err_unexpected_eof);
    /* With "ZZY" as extra input */
    static const ChunkCase zzy[] = {
        {{"NBSW", "Y3DP", "O5XX", "E3DE", "ZZY"}},
        {{"NBSWY3DPO5XXE3DE", "ZZY"}},
        {{"NBSWY3DPO5XXE3DEZZY"}},
        {{"NBS", "WY3", "DPO", "5XX", "E3D", "EZZY"}},
        {{"NBSWY3DPO5XXE3", "DEZZY"}},
    };
    same_error(t, a, "helloworld", zzy, sizeof zzy / sizeof zzy[0],
               io_err_unexpected_eof);
    /* The normal case, which is valid input */
    static const ChunkCase ok[] = {
        {{"NBSW", "Y3DP", "O5XX", "E3DE"}},
        {{"NBSWY3DPO5XXE3DE"}},
        {{"NBS", "WY3", "DPO", "5XX", "E3D", "E"}},
        {{"NBSWY3DPO5XXE3", "DE"}},
    };
    same_error(t, a, "helloworld", ok, sizeof ok / sizeof ok[0], BURROW_NO_ERROR);
    /* MZXW6YTB = fooba */
    static const ChunkCase fooba_zz[] = {
        {{"MZXW6YTBZZ"}},     {{"MZXW6YTBZ", "Z"}},     {{"MZXW6YTB", "ZZ"}},
        {{"MZXW6YT", "BZZ"}}, {{"MZXW6Y", "TBZZ"}},     {{"MZXW6Y", "TB", "ZZ"}},
        {{"MZXW6", "YTBZZ"}}, {{"MZXW6", "YTB", "ZZ"}}, {{"MZXW6", "YT", "BZZ"}},
    };
    same_error(t, a, "fooba", fooba_zz, sizeof fooba_zz / sizeof fooba_zz[0],
               io_err_unexpected_eof);
    static const ChunkCase fooba[] = {
        {{"MZXW6YTB"}},         {{"MZXW6YT", "B"}},     {{"MZXW6Y", "TB"}},
        {{"MZXW6", "YTB"}},     {{"MZXW6", "YT", "B"}}, {{"MZXW", "6YTB"}},
        {{"MZXW", "6Y", "TB"}},
    };
    same_error(t, a, "fooba", fooba, sizeof fooba / sizeof fooba[0], BURROW_NO_ERROR);
    arena_free(&ar);
}

static void TestBufferedDecodingPadding(TestingT *t) {
    static const struct {
        ChunkCase c;
        const char *want;
    } cases[] = {
        {{{"I4======", "=="}}, "unexpected EOF"},
        {{{"I4======N4======"}}, "illegal base32 data at input byte 2"},
        {{{"I4======", "N4======"}}, "illegal base32 data at input byte 0"},
        {{{"I4======", "========"}}, "illegal base32 data at input byte 0"},
        {{{"I4I4I4I4", "I4======", "I4======"}}, "illegal base32 data at input byte 0"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ChunkReader cr = {cases[i].c.chunks, count_chunks(&cases[i].c), 0, 0};
        IoReader decoder = base32_new_decoder(a, base32_std_encoding,
                                              (IoReader){&chunk_reader_vt, &cr});
        Error err;
        read_all(a, decoder, &err);
        Str want = str_from_cstr(cases[i].want);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "case %d: got nil error, want %s", (Int)i, want);
        else if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "case %d: got %s, want %s", (Int)i, error_text(err),
                               want);
    }
    arena_free(&ar);
}

typedef struct LenTest {
    bool raw;
    Int n;
    int64_t want;
} LenTest;

static void TestEncodedLen(TestingT *t) {
    const LenTest tests[] = {
        {false, 0, 0},
        {false, 1, 8},
        {false, 2, 8},
        {false, 3, 8},
        {false, 4, 8},
        {false, 5, 8},
        {false, 6, 16},
        {false, 10, 16},
        {false, 11, 24},
        {true, 0, 0},
        {true, 1, 2},
        {true, 2, 4},
        {true, 3, 5},
        {true, 4, 7},
        {true, 5, 8},
        {true, 6, 10},
        {true, 7, 12},
        {true, 10, 16},
        {true, 11, 18},
        /* overflow, for a 64 bit int */
        {true, (INT64_MAX - 4) / 8 + 1, 1844674407370955162},
        {true, INT64_MAX / 8 * 5 + 4, INT64_MAX},
    };
    Base32Encoding raw = raw_std();
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const Base32Encoding *enc = tests[i].raw ? &raw : base32_std_encoding;
        Int got = base32_encoding_encoded_len(enc, tests[i].n);
        if ((int64_t)got != tests[i].want)
            testing_t_errorf_v(t, "EncodedLen(%d): got %d, want %d", tests[i].n, got,
                               (Int)tests[i].want);
    }
}

static void TestDecodedLen(TestingT *t) {
    const LenTest tests[] = {
        {false, 0, 0},
        {false, 8, 5},
        {false, 16, 10},
        {false, 24, 15},
        {true, 0, 0},
        {true, 2, 1},
        {true, 4, 2},
        {true, 5, 3},
        {true, 7, 4},
        {true, 8, 5},
        {true, 10, 6},
        {true, 12, 7},
        {true, 16, 10},
        {true, 18, 11},
        /* overflow, for a 64 bit int */
        {true, INT64_MAX / 5 + 1, 1152921504606846976},
        {true, INT64_MAX, 5764607523034234879},
    };
    Base32Encoding raw = raw_std();
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const Base32Encoding *enc = tests[i].raw ? &raw : base32_std_encoding;
        Int got = base32_encoding_decoded_len(enc, tests[i].n);
        if ((int64_t)got != tests[i].want)
            testing_t_errorf_v(t, "DecodedLen(%d): got %d, want %d", tests[i].n, got,
                               (Int)tests[i].want);
    }
}

static void TestWithoutPaddingClose(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Base32Encoding encodings[] = {*base32_std_encoding, raw_std()};
    for (int k = 0; k < 2; k++) {
        const Base32Encoding *enc = &encodings[k];
        for (Int i = 0; i < N_PAIRS; i++) {
            StringsBuilder buf = STRINGS_BUILDER(a);
            IoWriteCloser encoder =
                base32_new_encoder(a, enc, strings_builder_as_io_writer(&buf));
            encoder.vt->writer.write(encoder.data, bytes_of(pairs[i].decoded), NULL);
            encoder.vt->closer.close(encoder.data);
            Str expected = pairs[i].encoded;
            if (enc->pad_char == BASE32_NO_PADDING)
                expected =
                    strings_replace_all(a, expected, BURROW_S("="), BURROW_S(""));
            Str res = strings_builder_string(&buf);
            if (!str_eq(res, expected))
                testing_t_errorf_v(t, "Expected %s got %s; padChar=%d", expected, res,
                                   (Int)enc->pad_char);
        }
    }
    arena_free(&ar);
}

static void TestDecodeReadAll(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Base32Encoding encodings[] = {*base32_std_encoding, raw_std()};
    for (Int i = 0; i < N_PAIRS; i++) {
        for (int k = 0; k < 2; k++) {
            const Base32Encoding *enc = &encodings[k];
            Str encoded = pairs[i].encoded;
            if (enc->pad_char == BASE32_NO_PADDING)
                encoded = strings_replace_all(a, encoded, BURROW_S("="), BURROW_S(""));
            StringsReader sr;
            strings_reader_reset(&sr, encoded);
            Error err;
            Slice got = read_all(
                a, base32_new_decoder(a, enc, strings_reader_as_io_reader(&sr)), &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "NewDecoder error: %s", error_text(err));
            if (!str_eq(pairs[i].decoded, str_of(got)))
                testing_t_errorf_v(t, "Expected %s got %s; Encoding %d",
                                   pairs[i].decoded, str_of(got), (Int)k);
        }
    }
    arena_free(&ar);
}

static void TestDecodeSmallBuffer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const Base32Encoding encodings[] = {*base32_std_encoding, raw_std()};
    for (Int size = 1; size < 200; size++) {
        for (Int i = 0; i < N_PAIRS; i++) {
            for (int k = 0; k < 2; k++) {
                const Base32Encoding *enc = &encodings[k];
                Str encoded = pairs[i].encoded;
                if (enc->pad_char == BASE32_NO_PADDING)
                    encoded =
                        strings_replace_all(a, encoded, BURROW_S("="), BURROW_S(""));
                StringsReader sr;
                strings_reader_reset(&sr, encoded);
                IoReader decoder =
                    base32_new_decoder(a, enc, strings_reader_as_io_reader(&sr));
                StringsBuilder all = STRINGS_BUILDER(a);
                Slice buf = slice_make(a, TYPE_BYTE, size, size);
                for (;;) {
                    Error err;
                    Int n = decoder.vt->read(decoder.data, buf, &err);
                    strings_builder_write(&all, slice_sub(buf, 0, n), NULL);
                    if (is_eof(err))
                        break;
                    if (BURROW_FAILED(err)) {
                        testing_t_errorf_v(t, "%s", error_text(err));
                        break;
                    }
                }
                Str got = strings_builder_string(&all);
                if (!str_eq(pairs[i].decoded, got))
                    testing_t_errorf_v(t,
                                       "Expected %s got %s; Encoding %d; bufferSize %d",
                                       pairs[i].decoded, got, (Int)k, size);
            }
        }
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- new tests */

/* The panic's text, copied out while the panic value is still there to read
 * it, since the value is gone once the block ends. */
static char panic_buf[256];

static Str recovered(Func f) {
    volatile Int n = 0;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        n = s.len < (Int)sizeof panic_buf ? s.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, s.p, (size_t)n);
    }
    BURROW_TRY_END;
    return str_from_bytes((const Byte *)panic_buf, n);
}

static void new_encoding(void *arg) {
    base32_new_encoding(*(Str *)arg);
}

static void with_padding(void *arg) {
    Rune r = *(Rune *)arg;
    base32_encoding_with_padding(base32_std_encoding, r);
}

static void TestNewEncodingPanics(TestingT *t) {
    static const struct {
        const char *alphabet;
        const char *want;
    } tests[] = {
        {"short", "encoding alphabet is not 32-bytes long"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZ23456\n",
         "encoding alphabet contains newline character"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZ23456\r",
         "encoding alphabet contains newline character"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZ23456A",
         "encoding alphabet includes duplicate symbols"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str s = str_from_cstr(tests[i].alphabet);
        Str v = recovered(BURROW_FN(Func, new_encoding, &s));
        if (!str_eq(v, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "NewEncoding(%q) panicked with %q, want %q", s, v,
                               str_from_cstr(tests[i].want));
    }
    static const struct {
        Rune pad;
        const char *want;
    } pads[] = {
        {'\n', "invalid padding"},
        {'\r', "invalid padding"},
        {0x100, "invalid padding"},
        {-2, "invalid padding"},
        {'A', "padding contained in alphabet"},
    };
    for (size_t i = 0; i < sizeof pads / sizeof pads[0]; i++) {
        Rune r = pads[i].pad;
        Str v = recovered(BURROW_FN(Func, with_padding, &r));
        if (!str_eq(v, str_from_cstr(pads[i].want)))
            testing_t_errorf_v(t, "WithPadding(%d) panicked with %q, want %q", (Int)r,
                               v, str_from_cstr(pads[i].want));
    }
    /* The hex alphabet works the same way. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Base32Encoding hex =
        base32_new_encoding(BURROW_S("0123456789ABCDEFGHIJKLMNOPQRSTUV"));
    Str s = base32_encoding_encode_to_string(&hex, a, BS("foobar"));
    CHECK(str_eq(s, BURROW_S("CPNMUOJ1E8======")));
    CHECK(str_eq(base32_encoding_encode_to_string(base32_hex_encoding, a, BS("foobar")),
                 s));
    arena_free(&ar);
}

/* Go takes a byte equal to byte(padChar) as padding, and for no padding that
 * is 0xff, so a 0xff near the end reads as the start of padding rather than
 * as a bad character. The offsets here are Go's. */
static void TestNoPaddingHighByte(TestingT *t) {
    Base32Encoding raw = raw_std();
    Byte dbuf[8];
    Slice dst = slice_from(dbuf, 5, 5, TYPE_BYTE);
    Error err;
    Int n = base32_encoding_decode(&raw, dst, BS("MZ\xffX"), &err);
    const Base32CorruptInputError *c = (const Base32CorruptInputError *)errors_as(
        err, TYPE_BASE32_CORRUPT_INPUT_ERROR);
    CHECK(n == 0 && c != NULL && *c == 4);
    n = base32_encoding_decode(&raw, dst, BS("MZ\xff"), &err);
    c = (const Base32CorruptInputError *)errors_as(err,
                                                   TYPE_BASE32_CORRUPT_INPUT_ERROR);
    CHECK(n == 0 && c != NULL && *c == 3);
    /* No padding and a few characters that are not a whole byte: nothing. */
    n = base32_encoding_decode(&raw, dst, BS("M"), &err);
    CHECK(n == 0 && BURROW_OK(err));
    n = base32_encoding_decode(&raw, dst, BS("MZX"), &err);
    CHECK(n == 0 && BURROW_OK(err));
}

typedef struct ShortCase {
    int how; /* 0 encode, 1 encode raw, 2 decode, 3 decode raw */
    const char *src;
    Int src_len;
    Int dst;
    const char *want;
} ShortCase;

static void run_short(void *arg) {
    const ShortCase *c = (const ShortCase *)arg;
    Byte dst[16];
    Slice d = slice_from(dst, c->dst, c->dst, TYPE_BYTE);
    Slice src =
        slice_from((void *)(uintptr_t)c->src, c->src_len, c->src_len, TYPE_BYTE);
    Base32Encoding raw = raw_std();
    const Base32Encoding *enc = c->how & 1 ? &raw : base32_std_encoding;
    if (c->how >= 2)
        base32_encoding_decode(enc, d, src, NULL);
    else
        base32_encoding_encode(enc, d, src);
}

/* What Go's index checks say for a dst that is too short, taken from Go. */
static void TestShortDst(TestingT *t) {
    static const Byte zeros[16] = {0};
    const char *z = (const char *)zeros;
    const ShortCase cases[] = {
        {0, z, 5, 0, "[7] with length 0"},
        {0, z, 5, 7, "[7] with length 7"},
        {0, z, 10, 12, "[7] with length 4"},
        {0, z, 1, 1, "[1] with length 1"},
        {0, z, 1, 2, "[2] with length 2"},
        {0, z, 1, 7, "[7] with length 7"},
        {0, z, 2, 3, "[3] with length 3"},
        {0, z, 2, 5, "[5] with length 5"},
        {0, z, 3, 4, "[4] with length 4"},
        {0, z, 3, 6, "[6] with length 6"},
        {0, z, 4, 6, "[6] with length 6"},
        {0, z, 4, 7, "[7] with length 7"},
        {0, z, 6, 9, "[1] with length 1"},
        {1, z, 1, 1, "[1] with length 1"},
        {1, z, 2, 3, "[3] with length 3"},
        {1, z, 3, 4, "[4] with length 4"},
        {1, z, 4, 6, "[6] with length 6"},
        {1, z, 4, 0, "[6] with length 0"},
        {2, "MZXW6YTB", 8, 4, "[4] with length 4"},
        {2, "MZXW6YTB", 8, 0, "[4] with length 0"},
        {2, "MZXW6YQ=", 8, 3, "[3] with length 3"},
        {2, "MZXW6===", 8, 2, "[2] with length 2"},
        {2, "MZXQ====", 8, 1, "[1] with length 1"},
        {2, "MY======", 8, 0, "[0] with length 0"},
        {2, "MZXW6YTBOI======", 16, 5, "[5] with length 5"},
        {2, "MZ\nXW6YTB", 9, 2, "[4] with length 2"},
        {3, "MZXW6YQ", 7, 3, "[3] with length 3"},
        {3, "MZXW6", 5, 2, "[2] with length 2"},
        {3, "MZXQ", 4, 1, "[1] with length 1"},
        {3, "MY", 2, 0, "[0] with length 0"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Str v = recovered(BURROW_FN(Func, run_short, (void *)(uintptr_t)&cases[i]));
        Str want = fmt_sprintf_v(a, "runtime error: index out of range %s",
                                 str_from_cstr(cases[i].want));
        if (!str_eq(v, want))
            testing_t_errorf_v(t, "#%d: panic %q, want %q", (Int)i, v, want);
    }
    /* Seven bytes are enough for "MZXW6YTBOI======" in Go, as the second block
     * writes only dst[5]. */
    Byte d7[7];
    Error err;
    Int n = base32_encoding_decode(base32_std_encoding, slice_from(d7, 7, 7, TYPE_BYTE),
                                   BS("MZXW6YTBOI======"), &err);
    CHECK(n == 6 && BURROW_OK(err) && memcmp(d7, "foobar", 6) == 0);
    arena_free(&ar);
}

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

/* After a failed write the encoder keeps the error and writes nothing more. */
static void TestWriteErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder sb = STRINGS_BUILDER(a);
    Limited l = {&sb, 2};
    IoWriteCloser w =
        base32_new_encoder(a, base32_std_encoding, (IoWriter){&limited_vt, &l});
    Error err;
    Int n = w.vt->writer.write(w.data, BS("foobar"), &err);
    CHECK(n == 0 && errors_is(err, limited_full));
    n = w.vt->writer.write(w.data, BS("x"), &err);
    CHECK(n == 0 && errors_is(err, limited_full));
    CHECK(errors_is(w.vt->closer.close(w.data), limited_full));
    CHECK(str_eq(strings_builder_string(&sb), BURROW_S("MZ")));
    arena_free(&ar);
}

static void TestCorruptInputErrorText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_eq(base32_corrupt_input_error_error(4, a),
                 BURROW_S("illegal base32 data at input byte 4")));
    CHECK(str_eq(base32_corrupt_input_error_error(-12, a),
                 BURROW_S("illegal base32 data at input byte -12")));
    Error e = base32_corrupt_input_error_as_error(INT64_MIN, a);
    CHECK(str_eq(error_text(e),
                 BURROW_S("illegal base32 data at input byte -9223372036854775808")));
    Error kept = error_retain(a, e);
    const Base32CorruptInputError *c = (const Base32CorruptInputError *)errors_as(
        kept, TYPE_BASE32_CORRUPT_INPUT_ERROR);
    CHECK(c != NULL && *c == INT64_MIN);
    arena_free(&ar);
}

static void TestNoMemory(TestingT *t) {
    static unsigned char room[8];
    Fixed fx;
    fixed_init(&fx, room, sizeof room);
    Alloc *a = fixed_allocator(&fx);
    Byte big[64] = {0};
    Slice src = slice_from(big, 64, 64, TYPE_BYTE);
    CHECK(base32_new_encoder(a, base32_std_encoding, (IoWriter){NULL, NULL}).vt ==
          NULL);
    CHECK(base32_new_decoder(a, base32_std_encoding, (IoReader){NULL, NULL}).vt ==
          NULL);
    CHECK(base32_encoding_encode_to_string(base32_std_encoding, a, src).len == 0);
    Error err;
    base32_encoding_decode_string(base32_std_encoding, a,
                                  BURROW_S("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
                                  &err);
    CHECK(errors_is(err, burrow_err_out_of_memory));
    base32_encoding_append_decode(base32_std_encoding, a, slice_nil(TYPE_BYTE), src,
                                  &err);
    CHECK(errors_is(err, burrow_err_out_of_memory));
    CHECK(base32_encoding_append_encode(base32_std_encoding, a, (Slice){0}, src).len ==
          0);
    CHECK(base32_corrupt_input_error_error(1234567890, a).len == 0);
    CHECK(errors_is(base32_corrupt_input_error_as_error(1234567890, a),
                    burrow_err_out_of_memory));
}

/* ------------------------------------------------------------ benchmarks */

static void BenchmarkEncode(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = slice_make(a, TYPE_BYTE, 8192, 8192);
    Int n = base32_encoding_encoded_len(base32_std_encoding, data.len);
    Slice buf = slice_make(a, TYPE_BYTE, n, n);
    testing_b_set_bytes(b, data.len);
    for (Int i = 0; i < testing_b_n(b); i++)
        base32_encoding_encode(base32_std_encoding, buf, data);
    arena_free(&ar);
}

static void BenchmarkEncodeToString(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = slice_make(a, TYPE_BYTE, 8192, 8192);
    testing_b_set_bytes(b, data.len);
    ArenaMark m = arena_mark(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        base32_encoding_encode_to_string(base32_std_encoding, a, data);
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

static void BenchmarkDecode(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = base32_encoding_encoded_len(base32_std_encoding, 8192);
    Slice data = slice_make(a, TYPE_BYTE, n, n);
    base32_encoding_encode(base32_std_encoding, data,
                           slice_make(a, TYPE_BYTE, 8192, 8192));
    Slice buf = slice_make(a, TYPE_BYTE, 8192, 8192);
    testing_b_set_bytes(b, data.len);
    for (Int i = 0; i < testing_b_n(b); i++)
        base32_encoding_decode(base32_std_encoding, buf, data, NULL);
    arena_free(&ar);
}

static void BenchmarkDecodeString(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str data = base32_encoding_encode_to_string(base32_std_encoding, a,
                                                slice_make(a, TYPE_BYTE, 8192, 8192));
    testing_b_set_bytes(b, data.len);
    ArenaMark m = arena_mark(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        base32_encoding_decode_string(base32_std_encoding, a, data, NULL);
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEncode)                                                                      \
    X(TestEncoder)                                                                     \
    X(TestEncoderBuffering)                                                            \
    X(TestDecoderBufferingWithPadding)                                                 \
    X(TestDecoderBufferingWithoutPadding)                                              \
    X(TestDecode)                                                                      \
    X(TestDecoder)                                                                     \
    X(TestIssue20044)                                                                  \
    X(TestDecoderError)                                                                \
    X(TestReaderEOF)                                                                   \
    X(TestDecoderBuffering)                                                            \
    X(TestDecodeCorrupt)                                                               \
    X(TestBig)                                                                         \
    X(TestNewLineCharacters)                                                           \
    X(TestDecoderIssue4779)                                                            \
    X(TestWithCustomPadding)                                                           \
    X(TestWithoutPadding)                                                              \
    X(TestDecodeWithPadding)                                                           \
    X(TestDecodeWithWrongPadding)                                                      \
    X(TestBufferedDecodingSameError)                                                   \
    X(TestBufferedDecodingPadding)                                                     \
    X(TestEncodedLen)                                                                  \
    X(TestDecodedLen)                                                                  \
    X(TestWithoutPaddingClose)                                                         \
    X(TestDecodeReadAll)                                                               \
    X(TestDecodeSmallBuffer)                                                           \
    X(TestNewEncodingPanics)                                                           \
    X(TestNoPaddingHighByte)                                                           \
    X(TestShortDst)                                                                    \
    X(TestWriteErrors)                                                                 \
    X(TestCorruptInputErrorText)                                                       \
    X(TestNoMemory)                                                                    \
    X(BenchmarkEncode)                                                                 \
    X(BenchmarkEncodeToString)                                                         \
    X(BenchmarkDecode)                                                                 \
    X(BenchmarkDecodeString)

TESTING_MAIN(TESTS)
