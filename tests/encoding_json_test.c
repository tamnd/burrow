/* encoding/json, the v1 API over v2: Marshal, Unmarshal and the v1 error
 * types, the formatting functions, and Number.
 *
 * Every expected output and error below is what go1.27.1 printed for the same
 * types written in Go, with the "main." in the type names dropped.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/map.h"
#include "burrow/math.h"
#include "burrow/mem/arena.h"

#include <string.h>

static Slice bytes_of(Alloc *a, const char *s) {
    return slice_append(a, slice_nil(TYPE_BYTE), s, (Int)strlen(s));
}

static Str cs(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

static Str q(Slice b) {
    return str_from_bytes(b.p, b.len);
}

static void check_str(TestingT *t, const char *name, Str got, const char *want) {
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "%s: got %q, want %q", name, got, want);
}

static void check_err(TestingT *t, const char *name, Error err, const char *want) {
    check_str(t, name, BURROW_FAILED(err) ? error_text(err) : BURROW_S(""), want);
}

/* ------------------------------------------------------------------ types */

#define INNER_FIELDS(F, T) F(T, Int, V, "json:\"v\"")
BURROW_STRUCT(Inner, INNER_FIELDS);

BURROW_SLICE_TYPE(Strs, Str);
BURROW_SLICE_TYPE(ByteSlice, uint8_t);
BURROW_SLICE_TYPE(Ints, Int);
BURROW_MAP_TYPE(StrIntMap, Str, Int);
BURROW_PTR_TYPE(InnerPtr, Inner);

#define ALL_FIELDS(F, T)                                                               \
    F(T, Str, Name, "")                                                                \
    F(T, Strs, Tags, "")                                                               \
    F(T, StrIntMap, M, "")                                                             \
    F(T, ByteSlice, B, "")                                                             \
    F(T, Str, Html, "")                                                                \
    F(T, Int, Skip, "json:\"-\"")                                                      \
    F(T, Str, Empty, "json:\",omitempty\"")                                            \
    F(T, Int, N, "json:\",string\"")                                                   \
    F(T, Inner, In, "")                                                                \
    F(T, InnerPtr, P, "")
BURROW_STRUCT(All, ALL_FIELDS);

/* MarshalJSON that fails. */
#define BOOM_FIELDS(F, T) F(T, Int, X, "")
BURROW_STRUCT_DECL(Boom, BOOM_FIELDS);

static Slice boom_marshal_json(Boom *b, Alloc *a, Error *err) {
    (void)b;
    (void)a;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("boom")));
    return slice_nil(TYPE_BYTE);
}

#define BOOM_METHODS(M, T) M(T, MarshalJSON, boom_marshal_json, JSONV2_SIG_MARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(Boom, BOOM_FIELDS, BOOM_METHODS);
BURROW_SLICE_TYPE(Booms, Boom);

/* MarshalJSON that writes something that is not JSON. */
#define BAD_FIELDS(F, T) F(T, Int, X, "")
BURROW_STRUCT_DECL(Bad, BAD_FIELDS);

static Slice bad_marshal_json(Bad *b, Alloc *a, Error *err) {
    (void)b;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_of(a, "{x");
}

#define BAD_METHODS(M, T) M(T, MarshalJSON, bad_marshal_json, JSONV2_SIG_MARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(Bad, BAD_FIELDS, BAD_METHODS);

/* MarshalText that fails. */
#define TXT_FIELDS(F, T) F(T, Int, X, "")
BURROW_STRUCT_DECL(Txt, TXT_FIELDS);

static Slice txt_marshal_text(Txt *x, Alloc *a, Error *err) {
    (void)x;
    (void)a;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("no text")));
    return slice_nil(TYPE_BYTE);
}

#define TXT_METHODS(M, T) M(T, MarshalText, txt_marshal_text, ENCODING_SIG_MARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Txt, TXT_FIELDS, TXT_METHODS);

#define SMALL_FIELDS(F, T)                                                             \
    F(T, int8_t, A, "")                                                                \
    F(T, Str, S, "")                                                                   \
    F(T, Ints, L, "")
BURROW_STRUCT(Small, SMALL_FIELDS);
BURROW_SLICE_TYPE(Smalls, Small);

#define ROOT_FIELDS(F, T) F(T, Smalls, Kids, "")
BURROW_STRUCT(Root, ROOT_FIELDS);

BURROW_ARRAY_TYPE(Byte2, uint8_t, 2);
BURROW_ARRAY_TYPE(Int2, Int, 2);
BURROW_MAP_TYPE(NumberIntMap, JsonNumber, Int);
BURROW_MAP_TYPE(IntStrMap, Int, Str);

#define NUMTAG_FIELDS(F, T) F(T, JsonNumber, N, "json:\",string\"")
BURROW_STRUCT(NumTag, NUMTAG_FIELDS);

typedef void *ChanInt;
static const Type burrow_type_ChanInt = {
    {NULL, 0},
    {NULL, 0},
    KIND_CHAN,
    (uint32_t)sizeof(ChanInt),
    _Alignof(ChanInt),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Int,
    NULL,
    0,
    0,
    NULL,
};
BURROW_MAP_TYPE(StrChanMap, Str, ChanInt);

/* ------------------------------------------------------------------ marshal */

static void check_marshal(TestingT *t, const char *name, Any v, const char *want,
                          const char *want_err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice out = json_marshal(a, v, &err);
    check_str(t, name, q(out), want);
    check_err(t, name, err, want_err);
    arena_free(&ar);
}

static void TestMarshal(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    All v;
    memset(&v, 0, sizeof(v));
    v.Name = BURROW_S("x");
    v.M = map_make(a, TYPE_OF(Str), TYPE_INT, 0);
    BURROW_MAP_SET(Str, Int, v.M, BURROW_S("b"), 1);
    BURROW_MAP_SET(Str, Int, v.M, BURROW_S("a"), 2);
    v.B = bytes_of(a, "hi");
    v.Html = BURROW_S("<a&b>");
    v.Skip = 9;
    v.N = 7;
    check_marshal(
        t, "struct", BURROW_ANY(TYPE_OF(All), &v),
        "{\"Name\":\"x\",\"Tags\":null,\"M\":{\"a\":2,\"b\":1},\"B\":\"aGk=\","
        "\"Html\":\"\\u003ca\\u0026b\\u003e\",\"N\":\"7\",\"In\":{\"v\":0},"
        "\"P\":null}",
        "");

    Byte2 b2 = {{1, 2}};
    check_marshal(t, "[2]byte", BURROW_ANY(TYPE_OF(Byte2), &b2), "[1,2]", "");
    arena_free(&ar);
}

static void TestMarshalIndent(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Inner in = {3};
    Slice out = json_marshal_indent(a, BURROW_ANY(TYPE_OF(Inner), &in), BURROW_S(">"),
                                    BURROW_S("\t"), &err);
    check_str(t, "indent", q(out), "{\n>\t\"v\": 3\n>}");
    check_err(t, "indent", err, "");

    Int two[2] = {1, 2};
    Slice s = {two, 2, 2, TYPE_INT};
    out = json_marshal_indent(a, BURROW_ANY(TYPE_OF(Ints), &s), BURROW_S(""),
                              BURROW_S("ab"), &err);
    check_str(t, "any indent", q(out), "[\nab1,\nab2\n]");
    check_err(t, "any indent", err, "");
    arena_free(&ar);
}

static void TestMarshalErrors(TestingT *t) {
    double nan = math_nan();
    check_marshal(t, "NaN", BURROW_ANY(TYPE_FLOAT64, &nan), "",
                  "json: unsupported value: NaN");
    double inf = math_inf(-1);
    check_marshal(t, "-Inf", BURROW_ANY(TYPE_FLOAT64, &inf), "",
                  "json: unsupported value: -Inf");

    int ch = 0;
    ChanInt c = &ch;
    check_marshal(t, "chan", BURROW_ANY(TYPE_OF(ChanInt), &c), "",
                  "json: unsupported type: chan int");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StrChanMap m = map_make(a, TYPE_OF(Str), TYPE_OF(ChanInt), 0);
    BURROW_MAP_SET(Str, ChanInt, m, BURROW_S("c"), c);
    check_marshal(t, "chan in map", BURROW_ANY(TYPE_OF(StrChanMap), &m), "",
                  "json: unsupported type: chan int");

    Boom boom = {1};
    check_marshal(t, "MarshalJSON", BURROW_ANY(TYPE_OF(Boom), &boom), "",
                  "json: error calling MarshalJSON for type *Boom: boom");
    Boom two[2] = {{1}, {2}};
    Slice booms = {two, 2, 2, TYPE_OF(Boom)};
    check_marshal(t, "[]Boom", BURROW_ANY(TYPE_OF(Booms), &booms), "",
                  "json: error calling MarshalJSON for type *Boom: boom");
    Bad bad = {1};
    check_marshal(
        t, "invalid MarshalJSON", BURROW_ANY(TYPE_OF(Bad), &bad), "",
        "json: error calling MarshalJSON for type *Bad: invalid character 'x' "
        "looking for beginning of object key string");
    Txt txt = {1};
    check_marshal(t, "MarshalText", BURROW_ANY(TYPE_OF(Txt), &txt), "",
                  "json: error calling MarshalText for type *Txt: no text");

    /* The types, and what they hold. */
    Error err = BURROW_NO_ERROR;
    (void)json_marshal(a, BURROW_ANY(TYPE_OF(Boom), &boom), &err);
    const JsonMarshalerError *me =
        (const JsonMarshalerError *)errors_as(err, TYPE_JSON_MARSHALER_ERROR);
    CHECK(me != NULL);
    if (me != NULL) {
        CHECK(me->type == TYPE_OF(Boom));
        check_str(t, "source_func", me->source_func, "MarshalJSON");
        check_err(t, "unwrap", json_marshaler_error_unwrap(me), "boom");
    }
    (void)json_marshal(a, BURROW_ANY(TYPE_FLOAT64, &nan), &err);
    const JsonUnsupportedValueError *uve = (const JsonUnsupportedValueError *)errors_as(
        err, TYPE_JSON_UNSUPPORTED_VALUE_ERROR);
    CHECK(uve != NULL && str_eq(uve->str, BURROW_S("NaN")));
    (void)json_marshal(a, BURROW_ANY(TYPE_OF(ChanInt), &c), &err);
    const JsonUnsupportedTypeError *ute = (const JsonUnsupportedTypeError *)errors_as(
        err, TYPE_JSON_UNSUPPORTED_TYPE_ERROR);
    CHECK(ute != NULL && ute->type == TYPE_OF(ChanInt));
    arena_free(&ar);
}

/* ---------------------------------------------------------------- unmarshal */

typedef struct UnmarshalCase {
    const char *in;
    const char *value;
    int64_t offset;
    const char *struct_name;
    const char *field;
    const char *msg;
} UnmarshalCase;

static void check_type_error(TestingT *t, const UnmarshalCase *c, Error err,
                             const Type *want_type) {
    check_err(t, c->in, err, c->msg);
    const JsonUnmarshalTypeError *te =
        (const JsonUnmarshalTypeError *)errors_as(err, TYPE_JSON_UNMARSHAL_TYPE_ERROR);
    if (te == NULL) {
        testing_t_errorf_v(t, "%s: not an UnmarshalTypeError", cs(c->in));
        return;
    }
    check_str(t, c->in, te->value, c->value);
    check_str(t, c->in, te->struct_name, c->struct_name);
    check_str(t, c->in, te->field, c->field);
    if (te->offset != c->offset)
        testing_t_errorf_v(t, "%s: offset %d, want %d", cs(c->in), te->offset,
                           c->offset);
    if (te->type != want_type)
        testing_t_errorf_v(t, "%s: wrong type", cs(c->in));
}

static void TestUnmarshalTypeErrors(TestingT *t) {
    static const UnmarshalCase small[] = {
        {"{\"A\":\"x\"}", "string", 8, "Small", "A",
         "json: cannot unmarshal string into Go struct field Small.A of type int8"},
        {"{\"A\":300}", "number 300", 8, "Small", "A",
         "json: cannot unmarshal number 300 into Go struct field Small.A of type int8"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof(small) / sizeof(small[0]); i++) {
        Small s;
        memset(&s, 0, sizeof(s));
        Error err =
            json_unmarshal(a, bytes_of(a, small[i].in), BURROW_ANY(TYPE_OF(Small), &s));
        check_type_error(t, &small[i], err, TYPE_OF(int8_t));
    }

    UnmarshalCase elem = {"{\"L\":[1,\"x\"]}",
                          "string",
                          11,
                          "Small",
                          "L.1",
                          "json: cannot unmarshal string into Small.L.1 of type int"};
    Small s;
    memset(&s, 0, sizeof(s));
    check_type_error(
        t, &elem,
        json_unmarshal(a, bytes_of(a, elem.in), BURROW_ANY(TYPE_OF(Small), &s)),
        TYPE_INT);

    static const UnmarshalCase root[] = {
        {"{\"Kids\":[{},{\"A\":true}]}", "bool", 21, "Root", "Kids.1.A",
         "json: cannot unmarshal bool into Go struct field Root.Kids.1.A of type int8"},
        {"{\"Kids\":[{\"L\":[1,{}]}]}", "object", 18, "Root", "Kids.0.L.1",
         "json: cannot unmarshal object into Root.Kids.0.L.1 of type int"},
    };
    for (size_t i = 0; i < sizeof(root) / sizeof(root[0]); i++) {
        Root r;
        memset(&r, 0, sizeof(r));
        Error err =
            json_unmarshal(a, bytes_of(a, root[i].in), BURROW_ANY(TYPE_OF(Root), &r));
        check_type_error(t, &root[i], err, i == 0 ? TYPE_OF(int8_t) : TYPE_INT);
    }

    Int n = 0;
    UnmarshalCase str = {
        "\"12\"", "string", 4,
        "",       "",       "json: cannot unmarshal string into Go value of type int"};
    check_type_error(t, &str,
                     json_unmarshal(a, bytes_of(a, str.in), BURROW_ANY(TYPE_INT, &n)),
                     TYPE_INT);
    UnmarshalCase frac = {
        "1.5", "number 1.5",
        3,     "",
        "",    "json: cannot unmarshal number 1.5 into Go value of type int"};
    check_type_error(t, &frac,
                     json_unmarshal(a, bytes_of(a, frac.in), BURROW_ANY(TYPE_INT, &n)),
                     TYPE_INT);

    IntStrMap m = NULL;
    UnmarshalCase key = {
        "{\"1\":\"a\",\"x\":\"b\"}",
        "number x",
        12,
        "",
        "x",
        "json: cannot unmarshal number x into Go struct field .x of type int"};
    check_type_error(
        t, &key,
        json_unmarshal(a, bytes_of(a, key.in), BURROW_ANY(TYPE_OF(IntStrMap), &m)),
        TYPE_INT);
    arena_free(&ar);
}

static void TestUnmarshalSyntaxErrors(TestingT *t) {
    static const struct {
        const char *in;
        int64_t offset;
        const char *msg;
    } cases[] = {
        {"{\"A\":}", 6, "invalid character '}' looking for beginning of value"},
        {"{\"A\":1", 6, "unexpected end of JSON input"},
        {"", 0, "unexpected end of JSON input"},
        {"{} x", 4, "invalid character 'x' after top-level value"},
        {"{1:2}", 2,
         "invalid character '1' looking for beginning of object key string"},
        {"{\"a\" 1}", 6, "invalid character '1' after object key"},
        {"{\"a\":1 \"b\":2}", 8, "invalid character '\"' after object key:value pair"},
        {"[1.e5]", 4, "invalid character 'e' in numeric literal"},
        {"tru", 3, "unexpected end of JSON input"},
        {"trux", 4, "invalid character 'x' in literal true (expecting 'e')"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Small s;
        memset(&s, 0, sizeof(s));
        Error err =
            json_unmarshal(a, bytes_of(a, cases[i].in), BURROW_ANY(TYPE_OF(Small), &s));
        check_err(t, cases[i].in, err, cases[i].msg);
        const JsonSyntaxError *se =
            (const JsonSyntaxError *)errors_as(err, TYPE_JSON_SYNTAX_ERROR);
        if (se == NULL) {
            testing_t_errorf_v(t, "%s: not a SyntaxError", cs(cases[i].in));
            continue;
        }
        if (se->offset != cases[i].offset)
            testing_t_errorf_v(t, "%s: offset %d, want %d", cs(cases[i].in), se->offset,
                               cases[i].offset);
        check_str(t, cases[i].in, json_syntax_error_error(se), cases[i].msg);
    }

    /* The whole input is checked before anything is stored. */
    Small s;
    memset(&s, 0, sizeof(s));
    Error err = json_unmarshal(a, bytes_of(a, "{\"A\":5,\"S\":"),
                               BURROW_ANY(TYPE_OF(Small), &s));
    check_err(t, "truncated", err, "unexpected end of JSON input");
    CHECK(s.A == 0);
    arena_free(&ar);
}

static void TestUnmarshal(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Small s;
    memset(&s, 0, sizeof(s));
    Error err =
        json_unmarshal(a, bytes_of(a, "{\"a\":5,\"s\":\"q\",\"l\":[1,2],\"zz\":1}"),
                       BURROW_ANY(TYPE_OF(Small), &s));
    check_err(t, "any case", err, "");
    CHECK(s.A == 5);
    CHECK(str_eq(s.S, BURROW_S("q")));
    CHECK(s.L.len == 2 && ((Int *)s.L.p)[0] == 1 && ((Int *)s.L.p)[1] == 2);

    Int2 arr = {{0, 0}};
    err = json_unmarshal(a, bytes_of(a, "[1,2,3]"), BURROW_ANY(TYPE_OF(Int2), &arr));
    check_err(t, "array", err, "");
    CHECK(arr.v[0] == 1 && arr.v[1] == 2);

    Any any = {NULL, NULL};
    err = json_unmarshal(a, bytes_of(a, "{\"x\":[1,\"a\",null,true]}"),
                         BURROW_ANY(TYPE_ANY, &any));
    check_err(t, "any", err, "");
    Slice back = json_marshal(a, any, &err);
    check_str(t, "any", q(back), "{\"x\":[1,\"a\",null,true]}");

    check_err(t, "nil", json_unmarshal(a, bytes_of(a, "{}"), BURROW_ANY(NULL, NULL)),
              "json: Unmarshal(nil)");
    err = json_unmarshal(a, bytes_of(a, "{}"), BURROW_ANY(TYPE_OF(Small), NULL));
    check_err(t, "nil *Small", err, "json: Unmarshal(nil *Small)");
    const JsonInvalidUnmarshalError *iue = (const JsonInvalidUnmarshalError *)errors_as(
        err, TYPE_JSON_INVALID_UNMARSHAL_ERROR);
    CHECK(iue != NULL && iue->type == TYPE_OF(Small));
    arena_free(&ar);
}

/* ------------------------------------------------------------------- Number */

static void TestNumber(TestingT *t) {
    JsonNumber n = BURROW_S("12.5");
    check_marshal(t, "number", BURROW_ANY(TYPE_JSON_NUMBER, &n), "12.5", "");
    n = BURROW_S("");
    check_marshal(t, "empty number", BURROW_ANY(TYPE_JSON_NUMBER, &n), "0", "");
    n = BURROW_S("abc");
    check_marshal(
        t, "bad number", BURROW_ANY(TYPE_JSON_NUMBER, &n), "",
        "json: error calling MarshalJSONTo for type *json.Number: cannot parse "
        "\"abc\" as JSON number: invalid syntax");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NumberIntMap m = map_make(a, TYPE_JSON_NUMBER, TYPE_INT, 0);
    BURROW_MAP_SET(JsonNumber, Int, m, BURROW_S("5"), 1);
    check_marshal(t, "number key", BURROW_ANY(TYPE_OF(NumberIntMap), &m), "{\"5\":1}",
                  "");
    NumTag nt = {BURROW_S("3")};
    check_marshal(t, "string tag", BURROW_ANY(TYPE_OF(NumTag), &nt), "{\"N\":\"3\"}",
                  "");

    Error err =
        json_unmarshal(a, bytes_of(a, "\"12\""), BURROW_ANY(TYPE_JSON_NUMBER, &n));
    check_err(t, "quoted", err, "");
    check_str(t, "quoted", json_number_string(n), "12");

    UnmarshalCase bad = {"\"x1\"",
                         "string \"x1\"",
                         0,
                         "",
                         "",
                         "json: cannot unmarshal string \"x1\" into Go value of type "
                         "json.Number: invalid syntax"};
    err = json_unmarshal(a, bytes_of(a, bad.in), BURROW_ANY(TYPE_JSON_NUMBER, &n));
    check_type_error(t, &bad, err, TYPE_JSON_NUMBER);
    CHECK(errors_is(err, strconv_err_syntax));
    err = json_unmarshal(a, bytes_of(a, "true"), BURROW_ANY(TYPE_JSON_NUMBER, &n));
    check_err(t, "bool", err,
              "json: cannot unmarshal bool into Go value of type json.Number");

    err = BURROW_NO_ERROR;
    CHECK(json_number_float64(BURROW_S("1e3"), &err) == 1000);
    check_err(t, "Float64", err, "");
    CHECK(json_number_int64(BURROW_S("1e3"), &err) == 0);
    check_err(t, "Int64", err, "strconv.ParseInt: parsing \"1e3\": invalid syntax");
    err = BURROW_NO_ERROR;
    CHECK(json_number_int64(BURROW_S("-42"), &err) == -42);
    check_err(t, "Int64", err, "");
    arena_free(&ar);
}

/* --------------------------------------------------------------- formatting */

static void TestValid(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(json_valid(bytes_of(a, "{\"a\":[1,2]}")));
    CHECK(!json_valid(bytes_of(a, "{\"a\":}")));
    CHECK(!json_valid(bytes_of(a, "")));
    CHECK(json_valid(bytes_of(a, " 1 ")));
    arena_free(&ar);
}

static void TestCompactIndent(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BytesBuffer b = BYTES_BUFFER(a);
    Error err = json_compact(&b, bytes_of(a, " { \"a\" : [ 1 , 2 ] } \n"));
    check_err(t, "Compact", err, "");
    check_str(t, "Compact", q(bytes_buffer_bytes(&b)), "{\"a\":[1,2]}");

    BytesBuffer keep = BYTES_BUFFER(a);
    bytes_buffer_write(&keep, bytes_of(a, "keep:"), &err);
    err = json_compact(&keep, bytes_of(a, "{\"a\":"));
    check_err(t, "Compact error", err, "unexpected end of JSON input");
    check_str(t, "Compact error", q(bytes_buffer_bytes(&keep)), "keep:");

    BytesBuffer ind = BYTES_BUFFER(a);
    err = json_indent(&ind, bytes_of(a, " {\"a\":[1,{}],\"b\":[]} \n"), BURROW_S("#"),
                      BURROW_S(".."));
    check_err(t, "Indent", err, "");
    check_str(t, "Indent", q(bytes_buffer_bytes(&ind)),
              "{\n#..\"a\": [\n#....1,\n#....{}\n#..],\n#..\"b\": []\n#} \n");

    BytesBuffer none = BYTES_BUFFER(a);
    err = json_indent(&none, bytes_of(a, "[1,2"), BURROW_S(""), BURROW_S("  "));
    check_err(t, "Indent error", err, "unexpected end of JSON input");
    check_str(t, "Indent error", q(bytes_buffer_bytes(&none)), "");

    BytesBuffer h = BYTES_BUFFER(a);
    json_html_escape(&h, bytes_of(a, "{\"x\":\"<b>&\xe2\x80\xa8\xe2\x80\xa9\"}"));
    check_str(t, "HTMLEscape", q(bytes_buffer_bytes(&h)),
              "{\"x\":\"\\u003cb\\u003e\\u0026\\u2028\\u2029\"}");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestMarshal)                                                                     \
    X(TestMarshalIndent)                                                               \
    X(TestMarshalErrors)                                                               \
    X(TestUnmarshalTypeErrors)                                                         \
    X(TestUnmarshalSyntaxErrors)                                                       \
    X(TestUnmarshal)                                                                   \
    X(TestNumber)                                                                      \
    X(TestValid)                                                                       \
    X(TestCompactIndent)

TESTING_MAIN(TESTS)
