/* encoding/json, the v1 streams: Decoder, Encoder and Token.
 *
 * Every expected output and error below is what go1.27.1 printed for the same
 * program written in Go, with the "main." in the type names dropped. Each
 * decoder test runs twice, once reading everything at once and once reading a
 * byte at a time, and has to give the same answers both ways.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/mem/arena.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define P_FIELDS(F, T)                                                                 \
    F(T, Int, A, "")                                                                   \
    F(T, Str, B, "json:\"b\"")
BURROW_STRUCT(P, P_FIELDS);

static Str cs(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

static void check_str(TestingT *t, const char *name, Str got, const char *want) {
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "%s: got %q, want %q", name, got, want);
}

static void check_err(TestingT *t, const char *name, Error err, const char *want) {
    check_str(t, name, BURROW_FAILED(err) ? error_text(err) : BURROW_S("<nil>"), want);
}

/* A reader over a string that hands out one byte per read when slow is set,
 * like Go's iotest.OneByteReader. */
typedef struct {
    Str s;
    Int i;
    bool slow;
} Src;

static Int src_read(void *self, Slice p, Error *err) {
    Src *r = (Src *)self;
    if (r->i >= r->s.len) {
        *err = io_eof;
        return 0;
    }
    Int n = r->s.len - r->i;
    if (n > p.len)
        n = p.len;
    if (r->slow && n > 1)
        n = 1;
    memcpy(p.p, r->s.p + r->i, (size_t)n);
    r->i += n;
    *err = BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT src_vt = {NULL, src_read};

static JsonDecoder *new_dec(Alloc *a, Src *src, const char *in, bool slow) {
    src->s = cs(in);
    src->i = 0;
    src->slow = slow;
    IoReader r = {&src_vt, src};
    return json_new_decoder(a, r);
}

/* A small text builder for the expected lines. */
typedef struct {
    char b[1024];
    size_t n;
} Line;

static void put(Line *l, const char *fmt, ...) BURROW_PRINTF(2, 3);
static void put(Line *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(l->b + l->n, sizeof l->b - l->n, fmt, ap);
    va_end(ap);
    if (n > 0)
        l->n += (size_t)n;
    if (l->n >= sizeof l->b)
        l->n = sizeof l->b - 1;
}

static Str marshal_any(Alloc *a, Any v) {
    Error err = BURROW_NO_ERROR;
    Slice b = json_marshal(a, v, &err);
    return BURROW_FAILED(err) ? error_text(err) : str_from_bytes(b.p, b.len);
}

static void TestDecodeStream(TestingT *t) {
    for (int slow = 0; slow < 2; slow++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Src src;
        JsonDecoder *d =
            new_dec(a, &src, "{\"A\":1,\"b\":\"x\"} [2,3] \"s\" 4.5 null", slow);
        Line l = {{0}, 0};
        for (;;) {
            Any v = {NULL, NULL};
            Error err = json_decoder_decode(d, BURROW_ANY(TYPE_ANY, &v));
            if (BURROW_FAILED(err)) {
                Str e = error_text(err);
                put(&l, "err=%.*s eof=%d off=%d", (int)e.len, (const char *)e.p,
                    errors_is(err, io_eof), (int)json_decoder_input_offset(d));
                break;
            }
            Str s = marshal_any(a, v);
            put(&l, "%.*s off=%d|", (int)s.len, (const char *)s.p,
                (int)json_decoder_input_offset(d));
        }
        check_str(t, "stream", str_from_bytes(l.b, (Int)l.n),
                  "{\"A\":1,\"b\":\"x\"} off=15|[2,3] off=21|\"s\" off=25|4.5 off=29|"
                  "null off=34|err=EOF eof=1 off=34");
        json_decoder_free(d);
        arena_free(&ar);
    }
}

static void TestDecodeErrors(TestingT *t) {
    static const struct {
        const char *in;
        const char *want[3];
        int want_a[3];
    } cases[] = {
        {"{\"A\":", {"unexpected EOF", "unexpected EOF", "unexpected EOF"}, {0, 0, 0}},
        {"{\"A\" 1}",
         {"invalid character '1' after object key",
          "invalid character '1' after object key",
          "invalid character '1' after object key"},
         {0, 0, 0}},
        {"[1,2] }",
         {"json: cannot unmarshal array into Go value of type P",
          "invalid character '}' looking for beginning of value",
          "invalid character '}' looking for beginning of value"},
         {0, 0, 0}},
        {"{\"A\":\"no\"} {\"A\":2}",
         {"json: cannot unmarshal string into Go struct field P.A of type int", "<nil>",
          "EOF"},
         {0, 2, 0}},
    };
    for (int slow = 0; slow < 2; slow++) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            Arena ar;
            arena_init(&ar, NULL, 0);
            Alloc *a = arena_allocator(&ar);
            Src src;
            JsonDecoder *d = new_dec(a, &src, cases[i].in, slow);
            for (int k = 0; k < 3; k++) {
                P p = {0};
                Error err = json_decoder_decode(d, BURROW_ANY(TYPE_OF(P), &p));
                check_err(t, cases[i].in, err, cases[i].want[k]);
                if (p.A != cases[i].want_a[k])
                    testing_t_errorf_v(t, "%s #%d: A = %d, want %d", cases[i].in, k,
                                       (int)p.A, cases[i].want_a[k]);
                if (k == 0 && i == 0 && !errors_is(err, io_err_unexpected_eof))
                    testing_t_errorf_v(t, "cut input: not io_err_unexpected_eof");
                if (errors_is(err, io_eof))
                    break;
            }
            json_decoder_free(d);
            arena_free(&ar);
        }
    }
}

static void TestDecodeOptions(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Src src;

    JsonDecoder *d = new_dec(a, &src, "{\"A\":1,\"c\":2}", false);
    json_decoder_disallow_unknown_fields(d);
    P p = {0};
    check_err(t, "unknown", json_decoder_decode(d, BURROW_ANY(TYPE_OF(P), &p)),
              "json: unknown field \"c\"");
    json_decoder_free(d);

    d = new_dec(a, &src, "{\"A\":10000000000000000000001} {\"A\":1.5}", false);
    json_decoder_use_number(d);
    Any v = {NULL, NULL};
    check_err(t, "usenumber", json_decoder_decode(d, BURROW_ANY(TYPE_ANY, &v)),
              "<nil>");
    Map *m = *(Map **)v.data;
    Str key = BURROW_S("A");
    const Any *got = map_get(m, &key);
    CHECK(got != NULL && got->t == TYPE_JSON_NUMBER);
    if (got != NULL && got->t == TYPE_JSON_NUMBER)
        check_str(t, "number", *(const JsonNumber *)got->data,
                  "10000000000000000000001");
    json_decoder_free(d);

    d = new_dec(a, &src, "{\"A\":1} tail bytes", false);
    check_err(t, "decode", json_decoder_decode(d, BURROW_ANY(TYPE_OF(P), &p)), "<nil>");
    IoReader rest = json_decoder_buffered(d);
    Error err = BURROW_NO_ERROR;
    Slice all = io_read_all(a, rest, &err);
    check_str(t, "buffered", str_from_bytes(all.p, all.len), " tail bytes");
    json_decoder_free(d);
    arena_free(&ar);
}

/* Reads tokens to the end the way the Go program did, writing each one as
 * %T(%v)@offset. */
static Str walk(Alloc *a, const char *in, bool use_number, bool slow) {
    Src src;
    JsonDecoder *d = new_dec(a, &src, in, slow);
    if (use_number)
        json_decoder_use_number(d);
    Line l = {{0}, 0};
    for (;;) {
        Error err = BURROW_NO_ERROR;
        JsonToken tok = json_decoder_token(d, &err);
        int off = (int)json_decoder_input_offset(d);
        if (BURROW_FAILED(err)) {
            Str e = error_text(err);
            put(&l, " err=%.*s off=%d", (int)e.len, (const char *)e.p, off);
            break;
        }
        if (tok.t == NULL) {
            put(&l, " nil@%d", off);
        } else if (tok.t == TYPE_JSON_DELIM) {
            put(&l, " %c@%d", (char)*(const JsonDelim *)tok.data, off);
        } else if (tok.t == TYPE_BOOL) {
            put(&l, " bool(%s)@%d", *(const bool *)tok.data ? "true" : "false", off);
        } else if (tok.t == TYPE_OF(Str)) {
            Str s = *(const Str *)tok.data;
            put(&l, " string(%.*s)@%d", (int)s.len, (const char *)s.p, off);
        } else if (tok.t == TYPE_JSON_NUMBER) {
            Str s = *(const JsonNumber *)tok.data;
            put(&l, " json.Number(%.*s)@%d", (int)s.len, (const char *)s.p, off);
        } else if (tok.t == TYPE_FLOAT64) {
            Str s = strconv_format_float(a, *(const double *)tok.data, 'g', -1, 64);
            put(&l, " float64(%.*s)@%d", (int)s.len, (const char *)s.p, off);
        } else {
            put(&l, " ?");
        }
    }
    json_decoder_free(d);
    return str_clone(a, str_from_bytes(l.b, (Int)l.n));
}

static void TestToken(TestingT *t) {
    static const struct {
        const char *in;
        bool use_number;
        const char *want;
    } cases[] = {
        {"{\"a\": [1, \"two\", true, null], \"b\": {}}", false,
         " {@1 string(a)@4 [@7 float64(1)@8 string(two)@15 bool(true)@21 nil@27 ]@28 "
         "string(b)@33 {@36 }@37 }@38 err=EOF off=38"},
        {"[1.50, -0, 1e1000]", true,
         " [@1 json.Number(1.50)@5 json.Number(-0)@9 json.Number(1e1000)@17 ]@18 "
         "err=EOF off=18"},
        {"[1e1000]", false,
         " [@1 err=json: cannot unmarshal number 1e1000 into Go value of type float64 "
         "off=7"},
        {"[1, 2", false, " [@1 float64(1)@2 float64(2)@5 err=EOF off=5"},
        {"[\"ab", false, " [@1 err=unexpected EOF off=1"},
        {"[1 2]", false,
         " [@1 float64(1)@2 err=invalid character '2' after array element off=2"},
        {" 1 \"x\" ", false, " float64(1)@2 string(x)@6 err=EOF off=6"},
    };
    for (int slow = 0; slow < 2; slow++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
            check_str(t, cases[i].in,
                      walk(a, cases[i].in, cases[i].use_number, slow != 0),
                      cases[i].want);
        arena_free(&ar);
    }
}

static void TestMore(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Src src;
    JsonDecoder *d = new_dec(a, &src, "[ 1 , {\"k\" : 2} ]", false);
    Error err = BURROW_NO_ERROR;
    (void)json_decoder_token(d, &err);
    Line l = {{0}, 0};
    while (json_decoder_more(d)) {
        put(&l, "more@%d ", (int)json_decoder_input_offset(d));
        Any v = {NULL, NULL};
        (void)json_decoder_decode(d, BURROW_ANY(TYPE_ANY, &v));
        Str s = marshal_any(a, v);
        put(&l, "%.*s@%d ", (int)s.len, (const char *)s.p,
            (int)json_decoder_input_offset(d));
    }
    JsonToken tok = json_decoder_token(d, &err);
    put(&l, "%c %d %d %d",
        tok.t == TYPE_JSON_DELIM ? (char)*(const JsonDelim *)tok.data : '?',
        BURROW_FAILED(err), json_decoder_more(d), (int)json_decoder_input_offset(d));
    check_str(t, "more", str_from_bytes(l.b, (Int)l.n),
              "more@2 1@3 more@4 {\"k\":2}@15 ] 0 0 17");
    json_decoder_free(d);

    /* Go's More says true at a cut, and the error comes from the next call. */
    d = new_dec(a, &src, "[1", false);
    (void)json_decoder_token(d, &err);
    (void)json_decoder_token(d, &err);
    CHECK(json_decoder_more(d));
    tok = json_decoder_token(d, &err);
    CHECK(tok.t == NULL);
    check_err(t, "then", err, "unexpected end of JSON input");
    json_decoder_free(d);
    arena_free(&ar);
}

/* A writer that takes the first write and fails every one after it. */
static Int fail_write(void *self, Slice p, Error *err) {
    int *n = (int *)self;
    if (++*n > 1) {
        *err = errors_new(error_allocator(), BURROW_S("disk full"));
        return 0;
    }
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT fail_vt = {NULL, fail_write};

BURROW_SLICE_TYPE(Ints, Int);
BURROW_MAP_TYPE(StrInt, Str, Int);

static void TestEncoder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    JsonEncoder *e = json_new_encoder(a, bytes_buffer_as_io_writer(&b));
    P p1 = {1, BURROW_S("<a&b>")};
    check_err(t, "encode", json_encoder_encode(e, BURROW_ANY(TYPE_OF(P), &p1)),
              "<nil>");
    json_encoder_set_escape_html(e, false);
    P p2 = {2, BURROW_S("<a&b>")};
    check_err(t, "encode", json_encoder_encode(e, BURROW_ANY(TYPE_OF(P), &p2)),
              "<nil>");
    json_encoder_set_indent(e, BURROW_S(">"), BURROW_S("\t"));
    Map *m = map_make(a, TYPE_OF(Str), TYPE_ANY, 2);
    Int nums[] = {1, 2};
    Slice z = {nums, 2, 2, TYPE_INT};
    Any zv = {TYPE_OF(Ints), &z};
    BURROW_MAP_SET(Str, Any, m, BURROW_S("z"), zv);
    Map *empty = map_make(a, TYPE_OF(Str), TYPE_INT, 0);
    Any av = {TYPE_OF(StrInt), &empty};
    BURROW_MAP_SET(Str, Any, m, BURROW_S("a"), av);
    check_err(t, "encode",
              json_encoder_encode(e, BURROW_ANY(TYPE_JSONV2_MAP_STRING_ANY, &m)),
              "<nil>");
    json_encoder_set_indent(e, BURROW_S(""), BURROW_S(""));
    Slice nil = slice_nil(TYPE_INT);
    check_err(t, "encode", json_encoder_encode(e, BURROW_ANY(TYPE_OF(Ints), &nil)),
              "<nil>");
    Slice out = bytes_buffer_bytes(&b);
    check_str(t, "encoder", str_from_bytes(out.p, out.len),
              "{\"A\":1,\"b\":\"\\u003ca\\u0026b\\u003e\"}\n"
              "{\"A\":2,\"b\":\"<a&b>\"}\n"
              "{\n>\t\"a\": {},\n>\t\"z\": [\n>\t\t1,\n>\t\t2\n>\t]\n>}\n"
              "null\n");
    json_encoder_free(e);

    int calls = 0;
    IoWriter w = {&fail_vt, &calls};
    e = json_new_encoder(a, w);
    Int one = 1;
    check_err(t, "w1", json_encoder_encode(e, BURROW_ANY(TYPE_INT, &one)), "<nil>");
    check_err(t, "w2", json_encoder_encode(e, BURROW_ANY(TYPE_INT, &one)), "disk full");
    check_err(t, "w3", json_encoder_encode(e, BURROW_ANY(TYPE_INT, &one)), "disk full");
    if (calls != 2)
        testing_t_errorf_v(t, "writer called %d times, want 2", calls);
    json_encoder_free(e);
    arena_free(&ar);
}

static void TestDelim(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    check_str(t, "{", json_delim_string('{', a), "{");
    check_str(t, "e", json_delim_string(0xe9, a), "\xc3\xa9");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDecodeStream)                                                                \
    X(TestDecodeErrors)                                                                \
    X(TestDecodeOptions)                                                               \
    X(TestToken)                                                                       \
    X(TestMore)                                                                        \
    X(TestEncoder)                                                                     \
    X(TestDelim)

TESTING_MAIN(TESTS)
