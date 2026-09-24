/* Derived from Go's src/encoding/base64/base64_test.go.
 * Go source: go1.27.1.
 *
 * Go's Encoding values are pointers and its helpers make new ones on the fly.
 * Here the made ones are plain values built once at the top of each test. The
 * tests at the end are new: the panics NewEncoding, WithPadding and a short
 * dst give, a writer that fails, and running out of memory.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/base64.h"
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
    /* RFC 3548 examples */
    TP("\x14\xfb\x9c\x03\xd9\x7e", "FPucA9l+"),
    TP("\x14\xfb\x9c\x03\xd9", "FPucA9k="),
    TP("\x14\xfb\x9c\x03", "FPucAw=="),

    /* RFC 4648 examples */
    TP("", ""),
    TP("f", "Zg=="),
    TP("fo", "Zm8="),
    TP("foo", "Zm9v"),
    TP("foob", "Zm9vYg=="),
    TP("fooba", "Zm9vYmE="),
    TP("foobar", "Zm9vYmFy"),

    /* Wikipedia examples */
    TP("sure.", "c3VyZS4="),
    TP("sure", "c3VyZQ=="),
    TP("sur", "c3Vy"),
    TP("su", "c3U="),
    TP("leasure.", "bGVhc3VyZS4="),
    TP("easure.", "ZWFzdXJlLg=="),
    TP("asure.", "YXN1cmUu"),
    TP("sure.", "c3VyZS4="),
};

#define N_PAIRS ((Int)(sizeof(pairs) / sizeof(pairs[0])))

static const TestPair bigtest = TP("Twas brillig, and the slithy toves",
                                   "VHdhcyBicmlsbGlnLCBhbmQgdGhlIHNsaXRoeSB0b3Zlcw==");

/* The reference string converters. */
enum { REF_STD, REF_URL, REF_RAW, REF_RAW_URL, REF_FUNNY };

static Str convert(Alloc *a, int how, Str ref) {
    Str s = ref;
    if (how == REF_URL || how == REF_RAW_URL) {
        s = strings_replace_all(a, s, BURROW_S("+"), BURROW_S("-"));
        s = strings_replace_all(a, s, BURROW_S("/"), BURROW_S("_"));
    }
    if (how == REF_RAW || how == REF_RAW_URL)
        s = strings_trim_right(s, BURROW_S("="));
    if (how == REF_FUNNY)
        s = strings_replace_all(a, s, BURROW_S("="), BURROW_S("@"));
    return s;
}

#define ENCODE_STD "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

typedef struct EncodingTest {
    Base64Encoding enc;
    int conv;
} EncodingTest;

#define N_ENCODINGS 10

/* Go's encodingTests: the four standard encodings, one with '@' for its
 * padding, and all five again strict. */
static void encoding_tests(EncodingTest *out) {
    Base64Encoding funny = base64_new_encoding(BURROW_S(ENCODE_STD));
    funny = base64_encoding_with_padding(&funny, '@');
    out[0] = (EncodingTest){*base64_std_encoding, REF_STD};
    out[1] = (EncodingTest){*base64_url_encoding, REF_URL};
    out[2] = (EncodingTest){*base64_raw_std_encoding, REF_RAW};
    out[3] = (EncodingTest){*base64_raw_url_encoding, REF_RAW_URL};
    out[4] = (EncodingTest){funny, REF_FUNNY};
    for (int i = 0; i < 5; i++)
        out[5 + i] = (EncodingTest){base64_encoding_strict(&out[i].enc), out[i].conv};
}

static void TestEncode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EncodingTest tests[N_ENCODINGS];
    encoding_tests(tests);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        for (int k = 0; k < N_ENCODINGS; k++) {
            const EncodingTest *tt = &tests[k];
            Str want = convert(a, tt->conv, p->encoded);
            Str got =
                base64_encoding_encode_to_string(&tt->enc, a, bytes_of(p->decoded));
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "Encode(%q) = %q, want %q", p->decoded, got,
                                   want);
            Slice dst = base64_encoding_append_encode(
                &tt->enc, a, slice_from_str(a, BURROW_S("lead")), bytes_of(p->decoded));
            Str lead = fmt_sprintf_v(a, "lead%s", want);
            if (!str_eq(str_of(dst), lead))
                testing_t_errorf_v(t, "AppendEncode(\"lead\", %q) = %q, want %q",
                                   p->decoded, str_of(dst), lead);
        }
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
        IoWriteCloser encoder = base64_new_encoder(a, base64_std_encoding,
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
        IoWriteCloser encoder = base64_new_encoder(a, base64_std_encoding,
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

static void TestDecode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EncodingTest tests[N_ENCODINGS];
    encoding_tests(tests);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        for (int k = 0; k < N_ENCODINGS; k++) {
            const EncodingTest *tt = &tests[k];
            Str encoded = convert(a, tt->conv, p->encoded);
            Int dlen = base64_encoding_decoded_len(&tt->enc, encoded.len);
            Slice dbuf = slice_make(a, TYPE_BYTE, dlen, dlen);
            Error err;
            Int count = base64_encoding_decode(&tt->enc, dbuf, bytes_of(encoded), &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "Decode(%q) = error %s, want nil", encoded,
                                   error_text(err));
            if (count != p->decoded.len)
                testing_t_errorf_v(t, "Decode(%q) = length %d, want %d", encoded, count,
                                   p->decoded.len);
            if (!str_eq(str_of(slice_sub(dbuf, 0, count)), p->decoded))
                testing_t_errorf_v(t, "Decode(%q) = %q, want %q", encoded,
                                   str_of(slice_sub(dbuf, 0, count)), p->decoded);

            dbuf = base64_encoding_decode_string(&tt->enc, a, encoded, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "DecodeString(%q) = error %s, want nil", encoded,
                                   error_text(err));
            if (!str_eq(str_of(dbuf), p->decoded))
                testing_t_errorf_v(t, "DecodeString(%q) = %q, want %q", encoded,
                                   str_of(dbuf), p->decoded);

            Slice dst = base64_encoding_append_decode(
                &tt->enc, a, slice_from_str(a, BURROW_S("lead")), bytes_of(encoded),
                &err);
            Str lead = fmt_sprintf_v(a, "lead%s", p->decoded);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "AppendDecode(%q) = error %s, want nil",
                                   p->encoded, error_text(err));
            if (!str_eq(str_of(dst), lead))
                testing_t_errorf_v(t, "AppendDecode(\"lead\", %q) = %q, want %q",
                                   p->encoded, str_of(dst), lead);

            /* Room for exactly the output, so no growth. */
            Slice room = slice_sub3(dst, 0, 0, p->decoded.len);
            Slice dst2 = base64_encoding_append_decode(&tt->enc, a, room,
                                                       bytes_of(encoded), &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "AppendDecode(%q) = error %s, want nil",
                                   p->encoded, error_text(err));
            if (!str_eq(str_of(dst2), p->decoded))
                testing_t_errorf_v(t, "AppendDecode(\"\", %q) = %q, want %q",
                                   p->encoded, str_of(dst2), p->decoded);
            if (dst.len > 0 && dst2.len > 0 && dst.p != dst2.p)
                testing_t_errorf_v(t, "unexpected capacity growth: got %d, want %d",
                                   dst2.cap, dst.cap);
        }
    }
    arena_free(&ar);
}

static bool is_eof(Error e) {
    return errors_is(e, io_eof);
}

static void TestDecoder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        StringsReader sr;
        strings_reader_reset(&sr, p->encoded);
        IoReader decoder = base64_new_decoder(a, base64_std_encoding,
                                              strings_reader_as_io_reader(&sr));
        Int dlen = base64_encoding_decoded_len(base64_std_encoding, p->encoded.len);
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

static void TestDecoderBuffering(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int bs = 1; bs <= 12; bs++) {
        StringsReader sr;
        strings_reader_reset(&sr, bigtest.encoded);
        IoReader decoder = base64_new_decoder(a, base64_std_encoding,
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
    CT("", -1),       CT("\n", -1),         CT("AAA=\n", -1), CT("AAAA\n", -1),
    CT("!!!!", 0),    CT("====", 0),        CT("x===", 1),    CT("=AAA", 0),
    CT("A=AA", 1),    CT("AA=A", 2),        CT("AA==A", 4),   CT("AAA=AAAA", 4),
    CT("AAAAA", 4),   CT("AAAAAA", 4),      CT("A=", 1),      CT("A==", 1),
    CT("AA=", 3),     CT("AA==", -1),       CT("AAA=", -1),   CT("AAAA", -1),
    CT("AAAAAA=", 7), CT("YWJjZA=====", 8), CT("A!\n", 1),    CT("A=\n", 1),
};

static void TestDecodeCorrupt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof corrupt_tests / sizeof corrupt_tests[0]; i++) {
        const CorruptTest *tc = &corrupt_tests[i];
        Int dlen = base64_encoding_decoded_len(base64_std_encoding, tc->input.len);
        Slice dbuf = slice_make(a, TYPE_BYTE, dlen, dlen);
        Error err;
        base64_encoding_decode(base64_std_encoding, dbuf, bytes_of(tc->input), &err);
        if (tc->offset == -1) {
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "Decoder wrongly detected corruption in %q",
                                   tc->input);
            continue;
        }
        const Base64CorruptInputError *c = (const Base64CorruptInputError *)errors_as(
            err, TYPE_BASE64_CORRUPT_INPUT_ERROR);
        if (c == NULL)
            testing_t_errorf_v(t, "Decoder failed to detect corruption in %q",
                               tc->input);
        else if (*c != tc->offset)
            testing_t_errorf_v(t, "Corruption in %q at offset %d, want %d", tc->input,
                               (Int)*c, tc->offset);
    }
    arena_free(&ar);
}

static void TestDecodeBounds(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte buf[32] = {0};
    Slice b = slice_from(buf, 32, 32, TYPE_BYTE);
    Str s = base64_encoding_encode_to_string(base64_std_encoding, a, b);
    Error err;
    Int n = base64_encoding_decode(base64_std_encoding, b, bytes_of(s), &err);
    if (n != 32 || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "StdEncoding.Decode = %d, %s, want %d, nil", n,
                           error_text(err), (Int)32);
    arena_free(&ar);
}

typedef struct LenTest {
    const Base64Encoding *const *enc;
    Int n;
    int64_t want;
} LenTest;

static void TestEncodedLen(TestingT *t) {
    const LenTest tests[] = {
        {&base64_raw_std_encoding, 0, 0},
        {&base64_raw_std_encoding, 1, 2},
        {&base64_raw_std_encoding, 2, 3},
        {&base64_raw_std_encoding, 3, 4},
        {&base64_raw_std_encoding, 7, 10},
        {&base64_std_encoding, 0, 0},
        {&base64_std_encoding, 1, 4},
        {&base64_std_encoding, 2, 4},
        {&base64_std_encoding, 3, 4},
        {&base64_std_encoding, 4, 8},
        {&base64_std_encoding, 7, 12},
        /* overflow, for a 64 bit int */
        {&base64_raw_std_encoding, (INT64_MAX - 5) / 8 + 1, 1537228672809129302},
        {&base64_raw_std_encoding, INT64_MAX / 4 * 3 + 2, INT64_MAX},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Int got = base64_encoding_encoded_len(*tests[i].enc, tests[i].n);
        if ((int64_t)got != tests[i].want)
            testing_t_errorf_v(t, "EncodedLen(%d): got %d, want %d", tests[i].n, got,
                               (Int)tests[i].want);
    }
}

static void TestDecodedLen(TestingT *t) {
    const LenTest tests[] = {
        {&base64_raw_std_encoding, 0, 0},
        {&base64_raw_std_encoding, 2, 1},
        {&base64_raw_std_encoding, 3, 2},
        {&base64_raw_std_encoding, 4, 3},
        {&base64_raw_std_encoding, 10, 7},
        {&base64_std_encoding, 0, 0},
        {&base64_std_encoding, 4, 3},
        {&base64_std_encoding, 8, 6},
        /* overflow, for a 64 bit int */
        {&base64_raw_std_encoding, INT64_MAX / 6 + 1, 1152921504606846976},
        {&base64_raw_std_encoding, INT64_MAX, 6917529027641081855},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Int got = base64_encoding_decoded_len(*tests[i].enc, tests[i].n);
        if ((int64_t)got != tests[i].want)
            testing_t_errorf_v(t, "DecodedLen(%d): got %d, want %d", tests[i].n, got,
                               (Int)tests[i].want);
    }
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
        base64_new_encoder(a, base64_std_encoding, bytes_buffer_as_io_writer(&encoded));
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
        base64_new_decoder(a, base64_std_encoding, bytes_buffer_as_io_reader(&encoded)),
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

static void TestNewLineCharacters(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Each of these should decode to the string "sure", without errors. */
    static const char *const examples[] = {
        "c3VyZQ==",     "c3VyZQ==\r",   "c3VyZQ==\n",       "c3VyZQ==\r\n",
        "c3VyZ\r\nQ==", "c3V\ryZ\nQ==", "c3V\nyZ\rQ==",     "c3VyZ\nQ==",
        "c3VyZQ\n==",   "c3VyZQ=\n=",   "c3VyZQ=\r\n\r\n=",
    };
    for (size_t i = 0; i < sizeof examples / sizeof examples[0]; i++) {
        Str e = str_from_cstr(examples[i]);
        Error err;
        Slice buf = base64_encoding_decode_string(base64_std_encoding, a, e, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Decode(%q) failed: %s", e, error_text(err));
            continue;
        }
        if (!str_eq(str_of(buf), BURROW_S("sure")))
            testing_t_errorf_v(t, "Decode(%q) = %q, want %q", e, str_of(buf),
                               BURROW_S("sure"));
    }
    arena_free(&ar);
}

/* Go's faultInjectReader, rate limited and with set errors, without the
 * channel: the reads it answers are listed up front. */
typedef struct NextRead {
    Int n;
    Error err;
} NextRead;

typedef struct FaultInjectReader {
    Str source;
    const NextRead *next;
    Int nnext;
    Int at;
} FaultInjectReader;

static Int fault_read(void *self, Slice p, Error *err) {
    FaultInjectReader *r = (FaultInjectReader *)self;
    if (r->at == r->nnext) {
        /* Go's reader would block here, and the test would time out. */
        *err = io_err_no_progress;
        return 0;
    }
    NextRead nr = r->next[r->at++];
    Int n = p.len < nr.n ? p.len : nr.n;
    if (n > r->source.len)
        n = r->source.len;
    memcpy(p.p, r->source.p, (size_t)n);
    r->source.p += n;
    r->source.len -= n;
    *err = nr.err;
    return n;
}

static const IoReaderVT fault_vt = {NULL, fault_read};

BURROW_SENTINEL_ERROR(my_error, "my error");

/* Tests that we don't ignore errors from our underlying reader. */
static void TestDecoderIssue3577(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const NextRead next[] = {{5, BURROW_NO_ERROR}, {10, my_error}, {0, my_error}};
    FaultInjectReader fr = {
        BURROW_S("VHdhcyBicmlsbGlnLCBhbmQgdGhlIHNsaXRoeSB0b3Zlcw=="), next, 3, 0};
    IoReader d = base64_new_decoder(a, base64_std_encoding, (IoReader){&fault_vt, &fr});
    Error err;
    read_all(a, d, &err);
    if (!errors_is(err, my_error))
        testing_t_errorf_v(t, "got error %s; want %s", error_text(err),
                           error_text(my_error));
    arena_free(&ar);
}

static void TestDecoderIssue4779(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str encoded = BURROW_S(
        "CP/EAT8AAAEF\n"
        "AQEBAQEBAAAAAAAAAAMAAQIEBQYHCAkKCwEAAQUBAQEBAQEAAAAAAAAAAQACAwQFBgcICQoLEAAB\n"
        "BAEDAgQCBQcGCAUDDDMBAAIRAwQhEjEFQVFhEyJxgTIGFJGhsUIjJBVSwWIzNHKC0UMHJZJT8OHx\n"
        "Y3M1FqKygyZEk1RkRcKjdDYX0lXiZfKzhMPTdePzRieUpIW0lcTU5PSltcXV5fVWZnaGlqa2xtbm\n"
        "9jdHV2d3h5ent8fX5/cRAAICAQIEBAMEBQYHBwYFNQEAAhEDITESBEFRYXEiEwUygZEUobFCI8FS\n"
        "0fAzJGLhcoKSQ1MVY3M08SUGFqKygwcmNcLSRJNUoxdkRVU2dGXi8rOEw9N14/NGlKSFtJXE1OT0\n"
        "pbXF1eX1VmZ2hpamtsbW5vYnN0dXZ3eHl6e3x//aAAwDAQACEQMRAD8A9VSSSSUpJJJJSkkkJ+Tj\n"
        "1kiy1jCJJDnAcCTykpKkuQ6p/jN6FgmxlNduXawwAzaGH+V6jn/R/wCt71zdn+N/qL3kVYFNYB4N\n"
        "ji6PDVjWpKp9TSXnvTf8bFNjg3qOEa2n6VlLpj/rT/pf567DpX1i6L1hs9Py67X8mqdtg/rUWbbf\n"
        "+gkp0kkkklKSSSSUpJJJJT//0PVUkkklKVLq3WMDpGI7KzrNjADtYNXvI/Mqr/Pd/q9W3vaxjnvM\n"
        "NaCXE9gNSvGPrf8AWS3qmba5jjsJhoB0DAf0NDf6sevf+/lf8Hj0JJATfWT6/dV6oXU1uOLQeKKn\n"
        "EQP+Hubtfe/+R7Mf/g7f5xcocp++Z11JMCJPgFBxOg7/AOuqDx8I/ikpkXkmSdU8mJIJA/O8EMAy\n"
        "j+mSARB/17pKVXYWHXjsj7yIex0PadzXMO1zT5KHoNA3HT8ietoGhgjsfA+CSnvvqh/jJtqsrwOv\n"
        "2b6NGNzXfTYexzJ+nU7/ALkf4P8Awv6P9KvTQQ4AgyDqCF85Pho3CTB7eHwXoH+LT65uZbX9X+o2\n"
        "bqbPb06551Y4\n");
    Str encoded_short = strings_replace_all(a, encoded, BURROW_S("\n"), BURROW_S(""));

    StringsReader sr;
    strings_reader_reset(&sr, encoded);
    Error err;
    Slice res1 = read_all(
        a, base64_new_decoder(a, base64_std_encoding, strings_reader_as_io_reader(&sr)),
        &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadAll failed: %s", error_text(err));

    StringsReader sr2;
    strings_reader_reset(&sr2, encoded_short);
    Slice res2 = read_all(
        a,
        base64_new_decoder(a, base64_std_encoding, strings_reader_as_io_reader(&sr2)),
        &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadAll failed: %s", error_text(err));

    if (!bytes_equal(res1, res2))
        testing_t_errorf_v(t, "Decoded results not equal");
    arena_free(&ar);
}

static void TestDecoderIssue7733(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Slice s = base64_encoding_decode_string(base64_std_encoding, a,
                                            BURROW_S("YWJjZA====="), &err);
    const Base64CorruptInputError *c = (const Base64CorruptInputError *)errors_as(
        err, TYPE_BASE64_CORRUPT_INPUT_ERROR);
    if (c == NULL || *c != 8)
        testing_t_errorf_v(t, "Error = %s; want CorruptInputError(8)", error_text(err));
    if (!str_eq(str_of(s), BURROW_S("abcd")))
        testing_t_errorf_v(t, "DecodeString = %q; want abcd", str_of(s));
    arena_free(&ar);
}

static void TestDecoderIssue15656(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Base64Encoding strict = base64_encoding_strict(base64_std_encoding);
    Error err;
    base64_encoding_decode_string(&strict, a, BURROW_S("WvLTlMrX9NpYDQlEIFlnDB=="),
                                  &err);
    const Base64CorruptInputError *c = (const Base64CorruptInputError *)errors_as(
        err, TYPE_BASE64_CORRUPT_INPUT_ERROR);
    if (c == NULL || *c != 22)
        testing_t_errorf_v(t, "Error = %s; want CorruptInputError(22)",
                           error_text(err));
    base64_encoding_decode_string(&strict, a, BURROW_S("WvLTlMrX9NpYDQlEIFlnDA=="),
                                  &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error = %s; want nil", error_text(err));
    base64_encoding_decode_string(base64_std_encoding, a,
                                  BURROW_S("WvLTlMrX9NpYDQlEIFlnDB=="), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error = %s; want nil", error_text(err));
    arena_free(&ar);
}

/* io.LimitReader, which the test needs to read in pieces of at most n. */
typedef struct LimitReader {
    IoReader r;
    Int n;
} LimitReader;

static Int limit_read(void *self, Slice p, Error *err) {
    LimitReader *l = (LimitReader *)self;
    if (l->n <= 0) {
        *err = io_eof;
        return 0;
    }
    if (p.len > l->n)
        p = slice_sub(p, 0, l->n);
    Int n = l->r.vt->read(l->r.data, p, err);
    l->n -= n;
    return n;
}

static const IoReaderVT limit_vt = {NULL, limit_read};

static void TestDecoderRaw(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str source = BURROW_S("AAAAAA");
    Slice want = BS("\x00\x00\x00\x00");

    /* Direct. */
    Error err;
    Slice dec1 =
        base64_encoding_decode_string(base64_raw_url_encoding, a, source, &err);
    if (BURROW_FAILED(err) || !bytes_equal(dec1, want))
        testing_t_errorf_v(t, "RawURLEncoding.DecodeString(%q) = %x, %s, want %x, nil",
                           source, str_of(dec1), error_text(err), str_of(want));

    /* Through reader. Used to fail. */
    BytesReader br;
    bytes_reader_reset(&br, bytes_of(source));
    IoReader r =
        base64_new_decoder(a, base64_raw_url_encoding, bytes_reader_as_io_reader(&br));
    LimitReader lr = {r, 100};
    Slice dec2 = read_all(a, (IoReader){&limit_vt, &lr}, &err);
    if (BURROW_FAILED(err) || !bytes_equal(dec2, want))
        testing_t_errorf_v(
            t, "reading NewDecoder(RawURLEncoding, %q) = %x, %s, want %x, nil", source,
            str_of(dec2), error_text(err), str_of(want));

    /* Should work with padding. */
    Str padded = BURROW_S("AAAAAA==");
    bytes_reader_reset(&br, bytes_of(padded));
    r = base64_new_decoder(a, base64_url_encoding, bytes_reader_as_io_reader(&br));
    Slice dec3 = read_all(a, r, &err);
    if (BURROW_FAILED(err) || !bytes_equal(dec3, want))
        testing_t_errorf_v(t,
                           "reading NewDecoder(URLEncoding, %q) = %x, %s, want %x, nil",
                           padded, str_of(dec3), error_text(err), str_of(want));
    arena_free(&ar);
}

/* ------------------------------------------------------------------- new */

/* The text of the panic f raises, copied out while the catch block still has
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
    base64_new_encoding(*(Str *)arg);
}

static void with_padding(void *arg) {
    Rune r = *(Rune *)arg;
    base64_encoding_with_padding(base64_std_encoding, r);
}

static void TestNewEncodingPanics(TestingT *t) {
    static const struct {
        const char *alphabet;
        const char *want;
    } tests[] = {
        {"short", "encoding alphabet is not 64-bytes long"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+\n",
         "encoding alphabet contains newline character"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+\r",
         "encoding alphabet contains newline character"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+A",
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
    /* A byte above 0x7f is written as that byte. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Base64Encoding high = base64_encoding_with_padding(base64_std_encoding, 0xff);
    Str s = base64_encoding_encode_to_string(&high, a, BS("f"));
    CHECK(str_eq(s, BURROW_S("Zg\xff\xff")));
    Error err;
    Slice back = base64_encoding_decode_string(&high, a, s, &err);
    CHECK(BURROW_OK(err) && str_eq(str_of(back), BURROW_S("f")));
    arena_free(&ar);
}

typedef struct ShortCase {
    bool decode;
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
    if (c->decode)
        base64_encoding_decode(base64_std_encoding, d, src, NULL);
    else
        base64_encoding_encode(base64_std_encoding, d, src);
}

/* What Go's index checks say for a dst that is too short, taken from Go. */
static void TestShortDst(TestingT *t) {
    static const Byte zeros[8] = {0};
    const char *z = (const char *)zeros;
    const ShortCase cases[] = {
        {false, z, 3, 0, "[3] with length 0"},
        {false, z, 3, 2, "[3] with length 2"},
        {false, z, 6, 5, "[3] with length 1"},
        {false, z, 6, 7, "[3] with length 3"},
        {false, z, 4, 5, "[1] with length 1"},
        {false, z, 4, 6, "[2] with length 2"},
        {false, z, 5, 7, "[3] with length 3"},
        {false, z, 1, 1, "[1] with length 1"},
        {false, z, 1, 3, "[3] with length 3"},
        {true, "QUJD", 4, 2, "[2] with length 2"},
        {true, "QUJD", 4, 1, "[2] with length 1"},
        {true, "QUI=", 4, 1, "[1] with length 1"},
        {true, "QQ==", 4, 0, "[0] with length 0"},
        {true, "QUJDRA==", 8, 3, "[0] with length 0"},
        {true, "QUJDREVGR0g=", 12, 6, "[1] with length 0"},
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
        base64_new_encoder(a, base64_std_encoding, (IoWriter){&limited_vt, &l});
    Error err;
    Int n = w.vt->writer.write(w.data, BS("foobar"), &err);
    CHECK(n == 0 && errors_is(err, limited_full));
    n = w.vt->writer.write(w.data, BS("x"), &err);
    CHECK(n == 0 && errors_is(err, limited_full));
    CHECK(errors_is(w.vt->closer.close(w.data), limited_full));
    CHECK(str_eq(strings_builder_string(&sb), BURROW_S("Zm")));
    arena_free(&ar);
}

static void TestCorruptInputErrorText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_eq(base64_corrupt_input_error_error(4, a),
                 BURROW_S("illegal base64 data at input byte 4")));
    CHECK(str_eq(base64_corrupt_input_error_error(-12, a),
                 BURROW_S("illegal base64 data at input byte -12")));
    Error e = base64_corrupt_input_error_as_error(INT64_MIN, a);
    CHECK(str_eq(error_text(e),
                 BURROW_S("illegal base64 data at input byte -9223372036854775808")));
    Error kept = error_retain(a, e);
    const Base64CorruptInputError *c = (const Base64CorruptInputError *)errors_as(
        kept, TYPE_BASE64_CORRUPT_INPUT_ERROR);
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
    CHECK(base64_new_encoder(a, base64_std_encoding, (IoWriter){NULL, NULL}).vt ==
          NULL);
    CHECK(base64_new_decoder(a, base64_std_encoding, (IoReader){NULL, NULL}).vt ==
          NULL);
    CHECK(base64_encoding_encode_to_string(base64_std_encoding, a, src).len == 0);
    Error err;
    base64_encoding_decode_string(base64_std_encoding, a,
                                  BURROW_S("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
                                  &err);
    CHECK(errors_is(err, burrow_err_out_of_memory));
    base64_encoding_append_decode(base64_std_encoding, a, slice_nil(TYPE_BYTE), src,
                                  &err);
    CHECK(errors_is(err, burrow_err_out_of_memory));
    CHECK(base64_encoding_append_encode(base64_std_encoding, a, (Slice){0}, src).len ==
          0);
    CHECK(base64_corrupt_input_error_error(1234567890, a).len == 0);
    CHECK(errors_is(base64_corrupt_input_error_as_error(1234567890, a),
                    burrow_err_out_of_memory));
}

/* ------------------------------------------------------------ benchmarks */

static void BenchmarkEncodeToString(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = slice_make(a, TYPE_BYTE, 8192, 8192);
    Slice sink = slice_make(a, TYPE_BYTE, 8192 / 3 * 4 + 4, 8192 / 3 * 4 + 4);
    testing_b_set_bytes(b, data.len);
    /* Into one buffer, so the arena does not grow with b.N. */
    for (Int i = 0; i < testing_b_n(b); i++)
        base64_encoding_encode(base64_std_encoding, sink, data);
    arena_free(&ar);
}

static void BenchmarkDecodeString(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice raw = slice_make(a, TYPE_BYTE, 8192, 8192);
    Str data = base64_encoding_encode_to_string(base64_std_encoding, a, raw);
    testing_b_set_bytes(b, data.len);
    for (Int i = 0; i < testing_b_n(b); i++)
        base64_encoding_decode(base64_std_encoding, raw, bytes_of(data), NULL);
    arena_free(&ar);
}

static void BenchmarkNewEncoding(TestingB *b) {
    testing_b_set_bytes(b, 256);
    for (Int i = 0; i < testing_b_n(b); i++) {
        volatile Base64Encoding e = base64_new_encoding(BURROW_S(ENCODE_STD));
        (void)e;
    }
}

#define TESTS(X)                                                                       \
    X(TestEncode)                                                                      \
    X(TestEncoder)                                                                     \
    X(TestEncoderBuffering)                                                            \
    X(TestDecode)                                                                      \
    X(TestDecoder)                                                                     \
    X(TestDecoderBuffering)                                                            \
    X(TestDecodeCorrupt)                                                               \
    X(TestDecodeBounds)                                                                \
    X(TestEncodedLen)                                                                  \
    X(TestDecodedLen)                                                                  \
    X(TestBig)                                                                         \
    X(TestNewLineCharacters)                                                           \
    X(TestDecoderIssue3577)                                                            \
    X(TestDecoderIssue4779)                                                            \
    X(TestDecoderIssue7733)                                                            \
    X(TestDecoderIssue15656)                                                           \
    X(TestDecoderRaw)                                                                  \
    X(TestNewEncodingPanics)                                                           \
    X(TestShortDst)                                                                    \
    X(TestWriteErrors)                                                                 \
    X(TestCorruptInputErrorText)                                                       \
    X(TestNoMemory)                                                                    \
    X(BenchmarkEncodeToString)                                                         \
    X(BenchmarkDecodeString)                                                           \
    X(BenchmarkNewEncoding)

TESTING_MAIN(TESTS)
