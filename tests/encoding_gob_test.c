/* Tests for encoding/gob that the generated table cannot carry: complex
 * numbers, types with GobEncode or MarshalBinary methods, registration,
 * interfaces, readers without ReadByte, the end of a stream and the nesting
 * limit. The expected bytes and messages come from Go 1.27 runs of the same
 * values.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/declare.h"
#include "burrow/encoding/gob.h"
#include "burrow/encoding/hex.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <string.h>

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, (Int)strlen(s)};
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : cstr("<nil>");
}

static void want_err(TestingT *t, const char *what, Error err, const char *want) {
    if (!str_eq(err_str(err), cstr(want)))
        testing_t_errorf_v(t, "%s: error = %q, want %q", cstr(what), err_str(err),
                           cstr(want));
}

/* Encodes v on its own and returns the stream as hex. */
static Str enc_hex(TestingT *t, Alloc *a, Any v) {
    BytesBuffer buf = BYTES_BUFFER(a);
    GobEncoder *e = gob_new_encoder(a, bytes_buffer_as_io_writer(&buf));
    Error err = gob_encoder_encode(e, v);
    gob_encoder_free(e);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Encode: %s", error_text(err));
    return hex_encode_to_string(a, bytes_buffer_bytes(&buf));
}

static Slice unhex(TestingT *t, Alloc *a, Str h) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, h, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "bad hex %s", h);
    return b;
}

static Error dec_bytes(Alloc *a, Slice data, Any v) {
    BytesReader br;
    bytes_reader_reset(&br, data);
    GobDecoder *d = gob_new_decoder(a, bytes_reader_as_io_reader(&br));
    Error err = gob_decoder_decode(d, v);
    gob_decoder_free(d);
    return err;
}

static void want_hex(TestingT *t, const char *what, Str got, const char *want) {
    if (!str_eq(got, cstr(want)))
        testing_t_errorf_v(t, "%s = %s, want %s", cstr(what), got, cstr(want));
}

/* ------------------------------------------------------------ complex */

#define CX_FIELDS(F, T)                                                                \
    F(T, Complex64, C, "")                                                             \
    F(T, Complex128, D, "")

BURROW_STRUCT(Cx, CX_FIELDS);

static void TestGobComplex(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Complex128 z = {1.5, 2.0};
    want_hex(t, "complex128", enc_hex(t, a, BURROW_ANY(TYPE_COMPLEX128, &z)),
             "060e00fef83f40");
    Complex128 back = {0, 0};
    want_err(t, "complex128",
             dec_bytes(a, unhex(t, a, BURROW_S("060e00fef83f40")),
                       BURROW_ANY(TYPE_COMPLEX128, &back)),
             "<nil>");
    if (back.re != z.re || back.im != z.im)
        testing_t_errorf_v(t, "complex128 round trip lost the value");

    const char *cx_hex = "1b7f03010102437801ff80000102010143010e00010144010e00000014ff"
                         "8001fef03ffef0bf01fee03ffb205fa0024200";
    Cx c = {{1.0F, -1.0F}, {0.5, 1e10}};
    want_hex(t, "Cx", enc_hex(t, a, BURROW_ANY(TYPE_OF(Cx), &c)), cx_hex);
    Cx c2 = {{0, 0}, {0, 0}};
    want_err(t, "Cx",
             dec_bytes(a, unhex(t, a, cstr(cx_hex)), BURROW_ANY(TYPE_OF(Cx), &c2)),
             "<nil>");
    if (c2.C.re != c.C.re || c2.C.im != c.C.im || c2.D.re != c.D.re ||
        c2.D.im != c.D.im)
        testing_t_errorf_v(t, "Cx round trip lost the value");
    arena_free(&ar);
}

/* ----------------------------------------------------------- GobEncode */

#define STAMP_FIELDS(F, T) F(T, Int, N, "")

BURROW_STRUCT_DECL(Stamp, STAMP_FIELDS);

static Slice stamp_gob_encode(Stamp *s, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice b = slice_append(a, slice_nil(TYPE_BYTE), "stamp:", 6);
    Str n = strconv_itoa(a, s->N);
    return slice_append(a, b, n.p, n.len);
}

static Error stamp_gob_decode(Stamp *s, Alloc *a, Slice b) {
    (void)a;
    bool found;
    Str rest =
        strings_cut_prefix(str_from_bytes(b.p, b.len), BURROW_S("stamp:"), &found);
    if (!found)
        return errors_new(error_allocator(), BURROW_S("stamp: bad prefix"));
    Error err = BURROW_NO_ERROR;
    s->N = strconv_atoi(rest, &err);
    return err;
}

#define STAMP_METHODS(M, T)                                                            \
    M(T, GobDecode, stamp_gob_decode, GOB_SIG_GOB_DECODE)                              \
    M(T, GobEncode, stamp_gob_encode, GOB_SIG_GOB_ENCODE)

BURROW_STRUCT_DEFINE_METHODS(Stamp, STAMP_FIELDS, STAMP_METHODS);

BURROW_PTR_TYPE(StampPtr, Stamp);

#define HOLDER_FIELDS(F, T)                                                            \
    F(T, Stamp, S, "")                                                                 \
    F(T, StampPtr, P, "")                                                              \
    F(T, Int, X, "")

BURROW_STRUCT(Holder, HOLDER_FIELDS);

/* Go's stream for Holder{Stamp{7}, &Stamp{8}, 9} with the types named
 * main.Holder and main.Stamp; the names here have no package, so the two
 * places that carry a name differ and the rest is the same. */
static const char holder_go[] =
    "28ff8103010106486f6c64657201ff8200010301015301ff840001015"
    "001ff8400010158010400000011ff83050101055374616d7001ff840"
    "0000017ff8201077374616d703a3701077374616d703a38011200";

static void TestGobGobEncoder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Stamp p8 = {8};
    Holder h = {{7}, &p8, 9};
    want_hex(t, "Holder", enc_hex(t, a, BURROW_ANY(TYPE_OF(Holder), &h)), holder_go);

    Holder h2 = {0};
    want_err(
        t, "Holder",
        dec_bytes(a, unhex(t, a, cstr(holder_go)), BURROW_ANY(TYPE_OF(Holder), &h2)),
        "<nil>");
    if (h2.S.N != 7 || h2.P == NULL || h2.P->N != 8 || h2.X != 9)
        testing_t_errorf_v(t, "Holder decoded wrong");

    /* The same stream with "stamp:7" turned into "stomp:7", which GobDecode
     * refuses; its error comes back as it is. */
    Slice bad = unhex(t, a, cstr(holder_go));
    Byte *p = bad.p;
    for (Int i = 0; i + 7 <= bad.len; i++)
        if (memcmp(p + i, "stamp:7", 7) == 0)
            p[i + 2] = 'o';
    Holder h3 = {0};
    want_err(t, "bad stamp", dec_bytes(a, bad, BURROW_ANY(TYPE_OF(Holder), &h3)),
             "stamp: bad prefix");

    /* A GobEncoder does not go into a plain type, nor the other way. */
    Stamp one = {1};
    Slice sb = unhex(t, a, enc_hex(t, a, BURROW_ANY(TYPE_OF(Stamp), &one)));
    Int n = 0;
    want_err(t, "stamp into int", dec_bytes(a, sb, BURROW_ANY(TYPE_INT, &n)),
             "gob: decoding into local type *int, received remote type Stamp");
    Int five = 5;
    Slice ib = unhex(t, a, enc_hex(t, a, BURROW_ANY(TYPE_INT, &five)));
    Stamp s = {0};
    want_err(t, "int into stamp", dec_bytes(a, ib, BURROW_ANY(TYPE_OF(Stamp), &s)),
             "gob: decoding into local type *Stamp, received remote type int");
    arena_free(&ar);
}

/* -------------------------------------------------------- MarshalBinary */

#define PAIR_FIELDS(F, T)                                                              \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")

BURROW_STRUCT_DECL(Pair, PAIR_FIELDS);

static Slice pair_marshal_binary(Pair *p, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Byte two[2] = {(Byte)p->X, (Byte)p->Y};
    return slice_append(a, slice_nil(TYPE_BYTE), two, 2);
}

static Error pair_unmarshal_binary(Pair *p, Alloc *a, Slice data) {
    (void)a;
    if (data.len != 2)
        return errors_new(error_allocator(), BURROW_S("pair: want two bytes"));
    p->X = ((const Byte *)data.p)[0];
    p->Y = ((const Byte *)data.p)[1];
    return BURROW_NO_ERROR;
}

#define PAIR_METHODS(M, T)                                                             \
    M(T, MarshalBinary, pair_marshal_binary, ENCODING_SIG_MARSHAL_BINARY)              \
    M(T, UnmarshalBinary, pair_unmarshal_binary, ENCODING_SIG_UNMARSHAL_BINARY)

BURROW_STRUCT_DEFINE_METHODS(Pair, PAIR_FIELDS, PAIR_METHODS);

static void TestGobBinaryMarshaler(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Go's stream for Pair{3, 4}. The type ids in it are Go's and need not
     * be the ones this process hands out, so check it by decoding it and
     * check our own stream by the payload at its end and a round trip. */
    Pair q = {0};
    want_err(t, "Go's Pair",
             dec_bytes(a,
                       unhex(t, a,
                             BURROW_S("10ff85060101045061697201ff860000000"
                                      "6ff8600020304")),
                       BURROW_ANY(TYPE_OF(Pair), &q)),
             "<nil>");
    if (q.X != 3 || q.Y != 4)
        testing_t_errorf_v(t, "Go's Pair decoded as {%d %d}", q.X, q.Y);
    Pair p = {3, 4};
    Str got = enc_hex(t, a, BURROW_ANY(TYPE_OF(Pair), &p));
    if (!strings_has_suffix(got, BURROW_S("00020304")))
        testing_t_errorf_v(t, "Pair = %s, want the payload 0304 at the end", got);
    q = (Pair){0};
    want_err(t, "Pair", dec_bytes(a, unhex(t, a, got), BURROW_ANY(TYPE_OF(Pair), &q)),
             "<nil>");
    if (q.X != 3 || q.Y != 4)
        testing_t_errorf_v(t, "Pair decoded as {%d %d}", q.X, q.Y);
    arena_free(&ar);
}

/* ------------------------------------------------ interfaces and names */

#define TT_FIELDS(F, T) F(T, Int, A, "")

BURROW_STRUCT(Tt, TT_FIELDS);

#define BOX_FIELDS(F, T) F(T, Any, I, "")

BURROW_STRUCT(Box, BOX_FIELDS);

static char panic_buf[256];

/* Runs gob_register_name and returns the panic text, or "" for none. */
static const char *try_register(Str name, Any v) {
    volatile int caught = 0;
    BURROW_TRY {
        gob_register_name(name, v);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        size_t m =
            s.len < (Int)sizeof panic_buf - 1 ? (size_t)s.len : sizeof panic_buf - 1;
        memcpy(panic_buf, s.p, m);
        panic_buf[m] = 0;
        caught = 1;
    }
    BURROW_TRY_END;
    return caught ? panic_buf : "";
}

static void TestGobInterface(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    const char *p = try_register(BURROW_S("main.T"), BURROW_ANY(TYPE_OF(Tt), NULL));
    if (*p != 0)
        testing_t_fatalf_v(t, "register: %s", cstr(p));
    /* Once more with the same pair is fine. */
    p = try_register(BURROW_S("main.T"), BURROW_ANY(TYPE_OF(Tt), NULL));
    if (*p != 0)
        testing_t_errorf_v(t, "register again: %s", cstr(p));
    p = try_register(BURROW_S("main.T"), BURROW_ANY(TYPE_OF(Pair), NULL));
    if (strcmp(p, "gob: registering duplicate types for \"main.T\": Tt != Pair") != 0)
        testing_t_errorf_v(t, "duplicate type: %q", cstr(p));
    p = try_register(BURROW_S("other.T"), BURROW_ANY(TYPE_OF(Tt), NULL));
    if (strcmp(p,
               "gob: registering duplicate names for Tt: \"main.T\" != \"other.T\"") !=
        0)
        testing_t_errorf_v(t, "duplicate name: %q", cstr(p));

    /* Box{T{7}} from Go, where T was registered as main.T and Box is main.Box. */
    const char *box_go = "17ff8703010103426f7801ff8800010101014901100000001fff8801066d6"
                         "1696e2e54ff89030101015401ff8a000101010141010400000007ff8a03"
                         "010e0000";
    Box b = {0};
    want_err(t, "Box",
             dec_bytes(a, unhex(t, a, cstr(box_go)), BURROW_ANY(TYPE_OF(Box), &b)),
             "<nil>");
    if (b.I.t != TYPE_OF(Tt) || ((Tt *)b.I.data)->A != 7)
        testing_t_errorf_v(t, "Box did not come back holding Tt{7}");

    /* Our own encoding of the same value decodes to the same thing. */
    Tt seven = {7};
    Box b1 = {BURROW_ANY(TYPE_OF(Tt), &seven)};
    Str own = enc_hex(t, a, BURROW_ANY(TYPE_OF(Box), &b1));
    Box b2 = {0};
    want_err(t, "own Box",
             dec_bytes(a, unhex(t, a, own), BURROW_ANY(TYPE_OF(Box), &b2)), "<nil>");
    if (b2.I.t != TYPE_OF(Tt) || ((Tt *)b2.I.data)->A != 7)
        testing_t_errorf_v(t, "own Box did not come back holding Tt{7}");

    /* A name nobody registered. */
    Slice unreg = unhex(t, a, cstr(box_go));
    Byte *q = unreg.p;
    for (Int i = 0; i + 6 <= unreg.len; i++)
        if (memcmp(q + i, "main.T", 6) == 0)
            q[i + 5] = 'U';
    Box b3 = {0};
    want_err(t, "unregistered", dec_bytes(a, unreg, BURROW_ANY(TYPE_OF(Box), &b3)),
             "gob: name not registered for interface: \"main.U\"");
    arena_free(&ar);
}

/* ------------------------------------------------------------- errors */

typedef struct NoByteReader {
    Slice data;
    Int off;
} NoByteReader;

/* Hands out at most three bytes a call and has no ReadByte, so the decoder
 * has to put its own buffer in front. */
static Int nbr_read(void *self, Slice p, Error *err) {
    NoByteReader *r = self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (r->off >= r->data.len) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    Int n = r->data.len - r->off;
    if (n > p.len)
        n = p.len;
    if (n > 3)
        n = 3;
    memcpy(p.p, (const Byte *)r->data.p + r->off, (size_t)n);
    r->off += n;
    return n;
}

static const IoReaderVT nbr_vt = {NULL, nbr_read};

static void TestGobStreams(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Two values through a reader with no ReadByte, then the end. */
    BytesBuffer buf = BYTES_BUFFER(a);
    GobEncoder *e = gob_new_encoder(a, bytes_buffer_as_io_writer(&buf));
    Int x = 5, y = -300;
    want_err(t, "encode x", gob_encoder_encode(e, BURROW_ANY(TYPE_INT, &x)), "<nil>");
    want_err(t, "encode y", gob_encoder_encode(e, BURROW_ANY(TYPE_INT, &y)), "<nil>");
    gob_encoder_free(e);
    NoByteReader r = {bytes_buffer_bytes(&buf), 0};
    GobDecoder *d = gob_new_decoder(a, (IoReader){&nbr_vt, &r});
    Int got = 0;
    want_err(t, "decode x", gob_decoder_decode(d, BURROW_ANY(TYPE_INT, &got)), "<nil>");
    if (got != 5)
        testing_t_errorf_v(t, "x = %d, want 5", got);
    want_err(t, "decode y", gob_decoder_decode(d, BURROW_ANY(TYPE_INT, &got)), "<nil>");
    if (got != -300)
        testing_t_errorf_v(t, "y = %d, want -300", got);
    Error err = gob_decoder_decode(d, BURROW_ANY(TYPE_INT, &got));
    if (!errors_is(err, io_eof))
        testing_t_errorf_v(t, "past the end: %s, want EOF", err_str(err));
    gob_decoder_free(d);

    /* An empty stream is a plain EOF, and one cut short is an unexpected EOF. */
    err = dec_bytes(a, slice_nil(TYPE_BYTE), BURROW_ANY(TYPE_INT, &got));
    if (!errors_is(err, io_eof))
        testing_t_errorf_v(t, "empty: %s, want EOF", err_str(err));
    Slice cut = bytes_buffer_bytes(&buf);
    cut.len = 2;
    err = dec_bytes(a, cut, BURROW_ANY(TYPE_INT, &got));
    if (!errors_is(err, io_err_unexpected_eof))
        testing_t_errorf_v(t, "cut: %s, want unexpected EOF", err_str(err));

    /* Values that cannot be encoded or decoded into. */
    BytesBuffer sink = BYTES_BUFFER(a);
    e = gob_new_encoder(a, bytes_buffer_as_io_writer(&sink));
    want_err(t, "nil", gob_encoder_encode(e, BURROW_ANY(NULL, NULL)),
             "gob: cannot encode nil value");
    gob_encoder_free(e);
    want_err(t, "unassignable",
             gob_decoder_decode_value(
                 gob_new_decoder(a, bytes_reader_as_io_reader(&(BytesReader){0})),
                 BURROW_ANY(TYPE_INT, NULL)),
             "gob: DecodeValue of unassignable value");
    arena_free(&ar);
}

/* A slice bigger than the decoder's first buffer, sent through the reader
 * with no ReadByte so it arrives three bytes at a time. */
static void TestGobLargeSlice(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice big = slice_make(a, TYPE_BYTE, 20000, 20000);
    for (Int i = 0; i < big.len; i++)
        ((Byte *)big.p)[i] = (Byte)i;
    BytesBuffer buf = BYTES_BUFFER(a);
    GobEncoder *e = gob_new_encoder(a, bytes_buffer_as_io_writer(&buf));
    want_err(t, "encode", gob_encoder_encode(e, BURROW_ANY(TYPE_BYTES, &big)), "<nil>");
    gob_encoder_free(e);
    if (bytes_buffer_len(&buf) != 20008)
        testing_t_errorf_v(t, "stream is %d bytes, want 20008 as in Go",
                           bytes_buffer_len(&buf));
    NoByteReader r = {bytes_buffer_bytes(&buf), 0};
    GobDecoder *d = gob_new_decoder(a, (IoReader){&nbr_vt, &r});
    Slice back = slice_nil(TYPE_BYTE);
    want_err(t, "decode", gob_decoder_decode(d, BURROW_ANY(TYPE_BYTES, &back)),
             "<nil>");
    if (back.len != big.len || memcmp(back.p, big.p, (size_t)big.len) != 0)
        testing_t_errorf_v(t, "large slice did not come back the same");
    gob_decoder_free(d);
    arena_free(&ar);
}

/* Go has no limit on nesting. Here the limit is the stack: a level that would
 * leave less than 64 KB of it is an error. A goroutine with a big stack
 * encodes and decodes 10,001 levels, as Go does, and the same stream on the
 * default stack is refused rather than overflowing it. */
static Any nest(Alloc *a, Int n) {
    Int *one = mem_alloc(a, sizeof(Int), _Alignof(Int));
    *one = 1;
    Any v = BURROW_ANY(TYPE_INT, one);
    for (Int i = 0; i < n; i++) {
        Box *b = mem_alloc(a, sizeof(Box), _Alignof(Box));
        b->I = v;
        v = BURROW_ANY(TYPE_OF(Box), b);
    }
    return v;
}

#if defined(BURROW_OS_WASI)

static Error enc_nest(Alloc *a, BytesBuffer *buf, Int n) {
    bytes_buffer_reset(buf);
    GobEncoder *e = gob_new_encoder(a, bytes_buffer_as_io_writer(buf));
    Error err = gob_encoder_encode(e, nest(a, n));
    gob_encoder_free(e);
    return err;
}

/* On wasip1 there are two stacks to run out of. The one in linear memory has
 * bounds, as anywhere else, but every goroutine also runs its calls on the
 * engine's stack, and nothing in the module can see how much of that is left,
 * so a count stands in for it. Go grows its stacks and takes ten thousand
 * levels here, and a fixed stack takes fewer. So the test finds how deep each
 * side goes, wants both to manage at least 100, and wants one level more to be
 * an error rather than a crash. The decoder spends more stack a level than the
 * encoder, so it stops sooner. */
static Error dec_nest(Alloc *a, BytesBuffer *buf, Int n, Int *levels) {
    Error err = enc_nest(a, buf, n);
    if (!BURROW_OK(err))
        return err;
    Box out = {0};
    err = dec_bytes(a, bytes_buffer_bytes(buf), BURROW_ANY(TYPE_OF(Box), &out));
    *levels = 0;
    for (Any v = BURROW_ANY(TYPE_OF(Box), &out); v.t == TYPE_OF(Box); (*levels)++)
        v = ((Box *)v.data)->I;
    return err;
}

static void TestGobNesting(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    gob_register_name(BURROW_S("main.Box"), BURROW_ANY(TYPE_OF(Box), NULL));

    BytesBuffer buf = BYTES_BUFFER(a);
    Int lo = 0, hi = 10001;
    while (hi - lo > 1) {
        Int mid = lo + (hi - lo) / 2;
        if (BURROW_OK(enc_nest(a, &buf, mid)))
            lo = mid;
        else
            hi = mid;
    }
    if (lo < 100)
        testing_t_errorf_v(t, "only %d levels encode, want at least 100", (int)lo);
    want_err(t, "encode one level too many", enc_nest(a, &buf, hi),
             "gob: encoder: nesting too deep");

    Int levels = 0;
    Int dlo = 0, dhi = lo + 1;
    while (dhi - dlo > 1) {
        Int mid = dlo + (dhi - dlo) / 2;
        if (BURROW_OK(dec_nest(a, &buf, mid, &levels)))
            dlo = mid;
        else
            dhi = mid;
    }
    testing_t_logf_v(t, "%d levels encode and %d decode", (int)lo, (int)dlo);
    if (dlo < 100)
        testing_t_errorf_v(t, "only %d levels decode, want at least 100", (int)dlo);
    if (dhi <= lo)
        want_err(t, "decode one level too many", dec_nest(a, &buf, dhi, &levels),
                 "gob: decoder: nesting too deep");
    want_err(t, "decode the deepest", dec_nest(a, &buf, dlo, &levels), "<nil>");
    if (levels != dlo)
        testing_t_errorf_v(t, "decoded %d levels, want %d", (int)levels, (int)dlo);
    arena_free(&ar);
}

#else

typedef struct DeepJob {
    Alloc *a;
    BytesBuffer *buf;
    Error enc;
    Error dec;
    Int levels;
    Chan *done;
} DeepJob;

static void deep_run(void *env) {
    DeepJob *j = env;
    GobEncoder *e = gob_new_encoder(j->a, bytes_buffer_as_io_writer(j->buf));
    j->enc = gob_encoder_encode(e, nest(j->a, 10001));
    gob_encoder_free(e);
    Box out = {0};
    j->dec =
        dec_bytes(j->a, bytes_buffer_bytes(j->buf), BURROW_ANY(TYPE_OF(Box), &out));
    for (Any v = BURROW_ANY(TYPE_OF(Box), &out); v.t == TYPE_OF(Box); j->levels++)
        v = ((Box *)v.data)->I;
    bool done = true;
    chan_send(j->done, &done);
}

static void TestGobNesting(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    gob_register_name(BURROW_S("main.Box"), BURROW_ANY(TYPE_OF(Box), NULL));

    BytesBuffer buf = BYTES_BUFFER(a);
    DeepJob j = {a, &buf, BURROW_NO_ERROR, BURROW_NO_ERROR, 0, NULL};
    j.done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (!go_stack(BURROW_FN(Func, deep_run, &j), (size_t)256 << 20))
        testing_t_fatalf_v(t, "go_stack failed");
    bool done;
    chan_recv(j.done, &done);
    chan_free(j.done);
    want_err(t, "encode 10001 on a big stack", j.enc, "<nil>");
    want_err(t, "decode 10001 on a big stack", j.dec, "<nil>");
    if (j.levels != 10001)
        testing_t_errorf_v(t, "decoded %d levels, want 10001", j.levels);

    Box out = {0};
    want_err(t, "decode 10001 here",
             dec_bytes(a, bytes_buffer_bytes(&buf), BURROW_ANY(TYPE_OF(Box), &out)),
             "gob: decoder: nesting too deep");
    BytesBuffer sink = BYTES_BUFFER(a);
    GobEncoder *e = gob_new_encoder(a, bytes_buffer_as_io_writer(&sink));
    want_err(t, "encode 10001 here", gob_encoder_encode(e, nest(a, 10001)),
             "gob: encoder: nesting too deep");
    gob_encoder_free(e);
    arena_free(&ar);
}

#endif

#define TESTS(X)                                                                       \
    X(TestGobComplex)                                                                  \
    X(TestGobGobEncoder)                                                               \
    X(TestGobBinaryMarshaler)                                                          \
    X(TestGobInterface)                                                                \
    X(TestGobStreams)                                                                  \
    X(TestGobLargeSlice)                                                               \
    X(TestGobNesting)

TESTING_MAIN(TESTS)
