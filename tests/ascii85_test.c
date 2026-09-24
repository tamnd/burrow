/* Derived from Go's src/encoding/ascii85/ascii85_test.go.
 * Go source: go1.27.1.
 *
 * The tests at the end are new: the panics a short dst gives, what Encode and
 * Decode leave behind in odd cases, a writer that fails, a reader that fails,
 * and running out of memory. Their expected values were taken from Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/ascii85.h"
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

static const TestPair bigtest = TP(
    "Man is distinguished, not only by his reason, but by this singular passion from "
    "other animals, which is a lust of the mind, that by a perseverance of delight in "
    "the continued and indefatigable generation of knowledge, exceeds the short "
    "vehemence of any carnal pleasure.",
    "9jqo^BlbD-BleB1DJ+*+F(f,q/0JhKF<GL>Cj@.4Gp$d7F!,L7@<6@)/0JDEF<G%<+EV:2F!,\n"
    "O<DJ+*.@<*K0@<6L(Df-\\0Ec5e;DffZ(EZee.Bl.9pF\"AGXBPCsi+DGm>@3BB/F*&OCAfu2/AKY\n"
    "i(DIb:@FD,*)+C]U=@3BN#EcYf8ATD3s@q?d$AftVqCh[NqF<G:8+EV:.+Cf>-FD5W8ARlolDIa\n"
    "l(DId<j@<?3r@:F%a+D58'ATD4$Bl@l3De:,-DJs`8ARoFb/0JMK@qB4^F!,R<AKZ&-DfTqBG%G\n"
    ">uD.RTpAKYo'+CT/5+Cei#DII?(E,9)oF*2M7/c\n");

static const TestPair pairs[] = {
    /* Encode returns 0 when len(src) is 0 */
    TP("", ""),
    /* Wikipedia example */
    bigtest,
    /* Special case when shortening !!!!! to z. */
    TP("\000\000\000\000", "z"),
};

#define N_PAIRS ((Int)(sizeof(pairs) / sizeof(pairs[0])))

static Str strip85(Alloc *a, Str s) {
    Slice t = slice_make(a, TYPE_BYTE, s.len, s.len);
    Int w = 0;
    for (Int r = 0; r < s.len; r++) {
        Byte c = s.p[r];
        if (c > ' ')
            ((Byte *)t.p)[w++] = c;
    }
    return str_of(slice_sub(t, 0, w));
}

static bool is_eof(Error e) {
    return errors_is(e, io_eof);
}

/* io.ReadAll, which is a copy until the end. */
static Slice read_all(Alloc *a, IoReader r, Error *err) {
    BytesBuffer out = BYTES_BUFFER(a);
    io_copy(a, bytes_buffer_as_io_writer(&out), r, err);
    return bytes_buffer_bytes(&out);
}

static void TestEncode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        Int m = ascii85_max_encoded_len(p->decoded.len);
        Slice buf = slice_make(a, TYPE_BYTE, m, m);
        Int n = ascii85_encode(buf, bytes_of(p->decoded));
        Str got = strip85(a, str_of(slice_sub(buf, 0, n)));
        Str want = strip85(a, p->encoded);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "Encode(%q) = %q, want %q", p->decoded, got, want);
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
        IoWriteCloser encoder =
            ascii85_new_encoder(a, strings_builder_as_io_writer(&bb));
        encoder.vt->writer.write(encoder.data, bytes_of(p->decoded), NULL);
        encoder.vt->closer.close(encoder.data);
        Str got = strip85(a, strings_builder_string(&bb));
        Str want = strip85(a, p->encoded);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "Encode(%q) = %q, want %q", p->decoded, got, want);
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
        IoWriteCloser encoder =
            ascii85_new_encoder(a, strings_builder_as_io_writer(&bb));
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
        Str got = strip85(a, strings_builder_string(&bb));
        Str want = strip85(a, bigtest.encoded);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "Encoding/%d of %q = %q, want %q", bs,
                               bigtest.decoded, got, want);
    }
    arena_free(&ar);
}

static void TestDecode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < N_PAIRS; i++) {
        const TestPair *p = &pairs[i];
        Int m = 4 * p->encoded.len;
        Slice dbuf = slice_make(a, TYPE_BYTE, m, m);
        Int nsrc;
        Error err;
        Int ndst = ascii85_decode(dbuf, bytes_of(p->encoded), true, &nsrc, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Decode(%q) = error %s, want nil", p->encoded,
                               error_text(err));
        if (nsrc != p->encoded.len)
            testing_t_errorf_v(t, "Decode(%q) = nsrc %d, want %d", p->encoded, nsrc,
                               p->encoded.len);
        if (ndst != p->decoded.len)
            testing_t_errorf_v(t, "Decode(%q) = ndst %d, want %d", p->encoded, ndst,
                               p->decoded.len);
        if (!str_eq(str_of(slice_sub(dbuf, 0, ndst)), p->decoded))
            testing_t_errorf_v(t, "Decode(%q) = %q, want %q", p->encoded,
                               str_of(slice_sub(dbuf, 0, ndst)), p->decoded);
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
        Error err;
        Slice dbuf =
            read_all(a, ascii85_new_decoder(a, strings_reader_as_io_reader(&sr)), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Read from %q = %s, want nil", p->encoded,
                               error_text(err));
        if (dbuf.len != p->decoded.len)
            testing_t_errorf_v(t, "Read from %q = length %d, want %d", p->encoded,
                               dbuf.len, p->decoded.len);
        if (!str_eq(str_of(dbuf), p->decoded))
            testing_t_errorf_v(t, "Decoding of %q = %q, want %q", p->encoded,
                               str_of(dbuf), p->decoded);
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
        IoReader decoder = ascii85_new_decoder(a, strings_reader_as_io_reader(&sr));
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

static void TestDecodeCorrupt(TestingT *t) {
    static const struct {
        const char *e;
        Int p;
    } examples[] = {
        {"v", 0},
        {"!z!!!!!!!!!", 1},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof examples / sizeof examples[0]; i++) {
        Str e = str_from_cstr(examples[i].e);
        Int m = 4 * e.len;
        Slice dbuf = slice_make(a, TYPE_BYTE, m, m);
        Error err;
        ascii85_decode(dbuf, bytes_of(e), true, NULL, &err);
        const Ascii85CorruptInputError *c = (const Ascii85CorruptInputError *)errors_as(
            err, TYPE_ASCII85_CORRUPT_INPUT_ERROR);
        if (c == NULL)
            testing_t_errorf_v(t, "Decoder failed to detect corruption in %q", e);
        else if (*c != examples[i].p)
            testing_t_errorf_v(t, "Corruption in %q at offset %d, want %d", e, (Int)*c,
                               examples[i].p);
    }
    arena_free(&ar);
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
    IoWriteCloser w = ascii85_new_encoder(a, bytes_buffer_as_io_writer(&encoded));
    Error err;
    Int nn = w.vt->writer.write(w.data, raw, &err);
    if (nn != n || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Encoder.Write(raw) = %d, %s want %d, nil", nn,
                           error_text(err), n);
    err = w.vt->closer.close(w.data);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Encoder.Close() = %s want nil", error_text(err));
    Slice decoded =
        read_all(a, ascii85_new_decoder(a, bytes_buffer_as_io_reader(&encoded)), &err);
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

static void TestDecoderInternalWhitespace(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str s = fmt_sprintf_v(a, "%sz", strings_repeat(a, BURROW_S(" "), 2048));
    StringsReader sr;
    strings_reader_reset(&sr, s);
    Error err;
    Slice decoded =
        read_all(a, ascii85_new_decoder(a, strings_reader_as_io_reader(&sr)), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Decode gave error %s", error_text(err));
    if (!str_eq(str_of(decoded), BURROW_S("\000\000\000\000")))
        testing_t_errorf_v(t, "Decode failed: got %q, want %q", str_of(decoded),
                           BURROW_S("\000\000\000\000"));
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

typedef struct ShortCase {
    Byte fill; /* every byte of src */
    Int src;
    Int dst;
    const char *want;
} ShortCase;

static void run_short(void *arg) {
    const ShortCase *c = (const ShortCase *)arg;
    Byte src[16], dst[16];
    memset(src, c->fill, sizeof src);
    ascii85_encode(slice_from(dst, c->dst, c->dst, TYPE_BYTE),
                   slice_from(src, c->src, c->src, TYPE_BYTE));
}

/* What Go's index checks say for a dst that is too short, taken from Go. */
static void TestShortDst(TestingT *t) {
    static const ShortCase cases[] = {
        {1, 4, 0, "[0] with length 0"}, {1, 4, 3, "[3] with length 3"},
        {1, 4, 4, "[4] with length 4"}, {1, 8, 7, "[2] with length 2"},
        {1, 8, 9, "[4] with length 4"}, {1, 1, 2, "[2] with length 2"},
        {1, 1, 4, "[4] with length 4"}, {1, 5, 5, "[0] with length 0"},
        {1, 5, 7, "[2] with length 2"}, {1, 3, 4, "[4] with length 4"},
        {0, 5, 5, "[4] with length 4"},
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

/* Encode writes whole groups into dst and counts only what it keeps. */
static void TestEncodeLeftovers(TestingT *t) {
    Byte d[10];
    memset(d, 0xAA, sizeof d);
    Int n = ascii85_encode(slice_from(d, 10, 10, TYPE_BYTE), BS("\001"));
    static const Byte one[10] = {33, 60, 60, 42, 34, 170, 170, 170, 170, 170};
    CHECK(n == 2 && memcmp(d, one, 10) == 0);
    memset(d, 0xAA, sizeof d);
    n = ascii85_encode(slice_from(d, 10, 10, TYPE_BYTE), BS("\0\0\0\0"));
    static const Byte zero[10] = {'z', 0, 0, 0, 0, 170, 170, 170, 170, 170};
    CHECK(n == 1 && memcmp(d, zero, 10) == 0);
    CHECK(ascii85_encode(slice_from(d, 0, 0, TYPE_BYTE), BS("")) == 0);
}

/* What Decode returns for a short dst, a bad tail, and a group that wraps,
 * each taken from Go. */
static void TestDecodeEdges(TestingT *t) {
    static const struct {
        const char *src;
        Int dst;
        bool flush;
        Int ndst, nsrc, err;
    } cases[] = {
        {"87cURD]i,\"Ebo80", 3, true, 0, 0, -1},
        {"87cURD]i,\"Ebo80", 8, false, 8, 10, -1},
        {"87cURD]i,\"Ebo80", 20, true, 12, 15, -1},
        {"87cUR~", 20, true, 0, 0, 5},
        {"87cURD", 20, true, 0, 0, 6},
        {"s8W-\"", 20, true, 4, 5, -1},
        {"uuuuu", 20, true, 4, 5, -1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Byte dst[20];
        Str src = str_from_cstr(cases[i].src);
        Int nsrc = -99;
        Error err;
        Int ndst =
            ascii85_decode(slice_from(dst, cases[i].dst, cases[i].dst, TYPE_BYTE),
                           bytes_of(src), cases[i].flush, &nsrc, &err);
        if (ndst != cases[i].ndst || nsrc != cases[i].nsrc)
            testing_t_errorf_v(t, "Decode(%q) = %d, %d, want %d, %d", src, ndst, nsrc,
                               cases[i].ndst, cases[i].nsrc);
        const Ascii85CorruptInputError *c = (const Ascii85CorruptInputError *)errors_as(
            err, TYPE_ASCII85_CORRUPT_INPUT_ERROR);
        if (cases[i].err < 0 ? BURROW_FAILED(err) : c == NULL || *c != cases[i].err)
            testing_t_errorf_v(t, "Decode(%q) error %s, want offset %d", src,
                               error_text(err), cases[i].err);
    }
    Byte dst[8];
    Int nd =
        ascii85_decode(slice_from(dst, 8, 8, TYPE_BYTE), BS("s8W-!"), true, NULL, NULL);
    CHECK(nd == 4 && memcmp(dst, "\xff\xff\xff\xff", 4) == 0);
    CHECK(ascii85_max_encoded_len(0) == 0 && ascii85_max_encoded_len(1) == 5 &&
          ascii85_max_encoded_len(4) == 5 && ascii85_max_encoded_len(5) == 10);
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
    IoWriteCloser w = ascii85_new_encoder(a, (IoWriter){&limited_vt, &l});
    Error err;
    Int n = w.vt->writer.write(w.data, BS("Man is"), &err);
    CHECK(n == 0 && errors_is(err, limited_full));
    n = w.vt->writer.write(w.data, BS("x"), &err);
    CHECK(n == 0 && errors_is(err, limited_full));
    CHECK(errors_is(w.vt->closer.close(w.data), limited_full));
    CHECK(str_eq(strings_builder_string(&sb), BURROW_S("9j")));

    /* A tail group that cannot be written is reported by Close. */
    StringsBuilder sb2 = STRINGS_BUILDER(a);
    Limited l2 = {&sb2, 1};
    w = ascii85_new_encoder(a, (IoWriter){&limited_vt, &l2});
    n = w.vt->writer.write(w.data, BS("Ma"), &err);
    CHECK(n == 2 && BURROW_OK(err));
    CHECK(errors_is(w.vt->closer.close(w.data), limited_full));
    CHECK(errors_is(w.vt->closer.close(w.data), limited_full));
    arena_free(&ar);
}

BURROW_SENTINEL_ERROR(read_broke, "broke");

typedef struct Failing {
    Str data;
} Failing;

static Int failing_read(void *self, Slice p, Error *err) {
    Failing *f = (Failing *)self;
    Int n = p.len < f->data.len ? p.len : f->data.len;
    memcpy(p.p, f->data.p, (size_t)n);
    f->data.p += n;
    f->data.len -= n;
    *err = f->data.len == 0 ? read_broke : BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT failing_vt = {NULL, failing_read};

/* The decoder hands out what it could decode, then the reader's error, and
 * keeps giving that error. A corrupt byte wins over the reader's error. What
 * each Read returns was taken from Go. */
static void TestReadErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Failing f = {BURROW_S("87cURD]")};
    IoReader r = ascii85_new_decoder(a, (IoReader){&failing_vt, &f});
    Byte buf[16];
    Error err;
    Int n = r.vt->read(r.data, slice_from(buf, 16, 16, TYPE_BYTE), &err);
    CHECK(n == 5 && BURROW_OK(err) && memcmp(buf, "Hello", 5) == 0);
    for (int i = 0; i < 3; i++) {
        n = r.vt->read(r.data, slice_from(buf, 16, 16, TYPE_BYTE), &err);
        CHECK(n == 0 && errors_is(err, read_broke));
    }
    n = r.vt->read(r.data, slice_from(buf, 0, 0, TYPE_BYTE), &err);
    CHECK(n == 0 && BURROW_OK(err));

    /* Decode gives nothing back when it finds a bad byte, so the good group
     * in front of it is lost too, as in Go. */
    Failing g = {BURROW_S("87cURv")};
    r = ascii85_new_decoder(a, (IoReader){&failing_vt, &g});
    for (int i = 0; i < 3; i++) {
        n = r.vt->read(r.data, slice_from(buf, 16, 16, TYPE_BYTE), &err);
        const Ascii85CorruptInputError *c = (const Ascii85CorruptInputError *)errors_as(
            err, TYPE_ASCII85_CORRUPT_INPUT_ERROR);
        CHECK(n == 0 && c != NULL && *c == 5);
    }
    arena_free(&ar);
}

static void TestCorruptInputErrorText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_eq(ascii85_corrupt_input_error_error(4, a),
                 BURROW_S("illegal ascii85 data at input byte 4")));
    CHECK(str_eq(ascii85_corrupt_input_error_error(-12, a),
                 BURROW_S("illegal ascii85 data at input byte -12")));
    Error e = ascii85_corrupt_input_error_as_error(INT64_MIN, a);
    CHECK(str_eq(error_text(e),
                 BURROW_S("illegal ascii85 data at input byte -9223372036854775808")));
    Error kept = error_retain(a, e);
    const Ascii85CorruptInputError *c = (const Ascii85CorruptInputError *)errors_as(
        kept, TYPE_ASCII85_CORRUPT_INPUT_ERROR);
    CHECK(c != NULL && *c == INT64_MIN);
    arena_free(&ar);
}

static void TestNoMemory(TestingT *t) {
    static unsigned char room[8];
    Fixed fx;
    fixed_init(&fx, room, sizeof room);
    Alloc *a = fixed_allocator(&fx);
    CHECK(ascii85_new_encoder(a, (IoWriter){NULL, NULL}).vt == NULL);
    CHECK(ascii85_new_decoder(a, (IoReader){NULL, NULL}).vt == NULL);
    CHECK(ascii85_corrupt_input_error_error(1234567890, a).len == 0);
    CHECK(errors_is(ascii85_corrupt_input_error_as_error(1234567890, a),
                    burrow_err_out_of_memory));
}

/* ------------------------------------------------------------ benchmarks */

static void BenchmarkEncode(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = slice_make(a, TYPE_BYTE, 8192, 8192);
    for (Int i = 0; i < data.len; i++)
        ((Byte *)data.p)[i] = (Byte)(i * 131 + 7);
    Int n = ascii85_max_encoded_len(data.len);
    Slice buf = slice_make(a, TYPE_BYTE, n, n);
    testing_b_set_bytes(b, data.len);
    for (Int i = 0; i < testing_b_n(b); i++)
        ascii85_encode(buf, data);
    arena_free(&ar);
}

static void BenchmarkDecode(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = slice_make(a, TYPE_BYTE, 8192, 8192);
    for (Int i = 0; i < data.len; i++)
        ((Byte *)data.p)[i] = (Byte)(i * 131 + 7);
    Int n = ascii85_max_encoded_len(data.len);
    Slice enc = slice_make(a, TYPE_BYTE, n, n);
    enc = slice_sub(enc, 0, ascii85_encode(enc, data));
    testing_b_set_bytes(b, data.len);
    for (Int i = 0; i < testing_b_n(b); i++)
        ascii85_decode(data, enc, true, NULL, NULL);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEncode)                                                                      \
    X(TestEncoder)                                                                     \
    X(TestEncoderBuffering)                                                            \
    X(TestDecode)                                                                      \
    X(TestDecoder)                                                                     \
    X(TestDecoderBuffering)                                                            \
    X(TestDecodeCorrupt)                                                               \
    X(TestBig)                                                                         \
    X(TestDecoderInternalWhitespace)                                                   \
    X(TestShortDst)                                                                    \
    X(TestEncodeLeftovers)                                                             \
    X(TestDecodeEdges)                                                                 \
    X(TestWriteErrors)                                                                 \
    X(TestReadErrors)                                                                  \
    X(TestCorruptInputErrorText)                                                       \
    X(TestNoMemory)                                                                    \
    X(BenchmarkEncode)                                                                 \
    X(BenchmarkDecode)

TESTING_MAIN(TESTS)
