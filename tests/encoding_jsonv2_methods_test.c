/* The methods json v2 calls in place of its own rules: MarshalJSONTo,
 * MarshalJSON, AppendText and MarshalText on the way out, UnmarshalJSONFrom,
 * UnmarshalJSON and UnmarshalText on the way in, and IsZero for omitzero.
 *
 * Go's own tests for these use types the test generator cannot describe yet,
 * so these are written by hand. Every expected output and error below is what
 * go1.27.1 printed for the same types written in Go, with the "main." in the
 * type names dropped.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json/jsontext.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <string.h>

static Slice bytes_of(Alloc *a, Slice b, const char *s) {
    return slice_append(a, b, s, (Int)strlen(s));
}

static Error boom(void) {
    return errors_new(error_allocator(), BURROW_S("boom"));
}

/* ------------------------------------------------------------------ types */

/* MarshalJSON and UnmarshalJSON, as {"c":deg}. */
#define CELSIUS_FIELDS(F, T) F(T, double, Deg, "")
BURROW_STRUCT_DECL(Celsius, CELSIUS_FIELDS);

#define CWIRE_FIELDS(F, T) F(T, double, C, "json:\"c\"")
BURROW_STRUCT_DECL(CelsiusWire, CWIRE_FIELDS);
BURROW_STRUCT_DEFINE(CelsiusWire, CWIRE_FIELDS);

static Slice celsius_marshal_json(Celsius *c, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str deg = strconv_format_float(a, c->Deg, 'g', -1, 64);
    Slice b = bytes_of(a, slice_nil(TYPE_BYTE), "{\"c\":");
    b = slice_append(a, b, deg.p, deg.len);
    return bytes_of(a, b, "}");
}

static Error celsius_unmarshal_json(Celsius *c, Alloc *a, Slice b) {
    CelsiusWire x = {0};
    Error err = jsonv2_unmarshal(a, b, BURROW_ANY(TYPE_OF(CelsiusWire), &x),
                                 slice_nil(TYPE_JSONTEXT_OPTIONS));
    if (BURROW_FAILED(err))
        return err;
    c->Deg = x.C;
    return BURROW_NO_ERROR;
}

#define CELSIUS_METHODS(M, T)                                                          \
    M(T, MarshalJSON, celsius_marshal_json, JSONV2_SIG_MARSHAL_JSON)                   \
    M(T, UnmarshalJSON, celsius_unmarshal_json, JSONV2_SIG_UNMARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(Celsius, CELSIUS_FIELDS, CELSIUS_METHODS);

/* MarshalText and UnmarshalText, as "x,y". */
#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT_DECL(Point, POINT_FIELDS);

static Slice point_marshal_text(Point *p, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "%d,%d", p->X, p->Y);
    return slice_append(a, slice_nil(TYPE_BYTE), s.p, s.len);
}

static Error point_unmarshal_text(Point *p, Alloc *a, Slice text) {
    (void)a;
    Str y;
    bool found;
    Str x = strings_cut(str_from_bytes(text.p, text.len), BURROW_S(","), &y, &found);
    if (!found)
        return errors_new(error_allocator(), BURROW_S("no comma"));
    Error err = BURROW_NO_ERROR;
    p->X = strconv_atoi(x, &err);
    p->Y = strconv_atoi(y, &err);
    return BURROW_NO_ERROR;
}

#define POINT_METHODS(M, T)                                                            \
    M(T, MarshalText, point_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                   \
    M(T, UnmarshalText, point_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);

typedef Point *PointPtr;
static const Type burrow_type_PointPtr = {
    BURROW_S_INIT(""),
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(PointPtr),
    _Alignof(PointPtr),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Point,
    NULL,
    0,
    0,
    NULL,
};

/* AppendText, as "0x" and the number in hex. */
#define HEX_FIELDS(F, T) F(T, Int, N, "")
BURROW_STRUCT_DECL(Hex, HEX_FIELDS);

static Slice hex_append_text(Hex *h, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return strconv_append_int(a, bytes_of(a, b, "0x"), h->N, 16);
}

#define HEX_METHODS(M, T) M(T, AppendText, hex_append_text, ENCODING_SIG_APPEND_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Hex, HEX_FIELDS, HEX_METHODS);

/* MarshalJSONTo and UnmarshalJSONFrom, upper case on the wire and lower case
 * in memory. */
#define UPPER_FIELDS(F, T) F(T, Str, S, "")
BURROW_STRUCT_DECL(Upper, UPPER_FIELDS);

static Error upper_marshal_json_to(Upper *u, Jsonv2EncoderArg e) {
    Byte buf[64];
    Int n = u->S.len < (Int)sizeof(buf) ? u->S.len : (Int)sizeof(buf);
    for (Int i = 0; i < n; i++) {
        Byte c = u->S.p[i];
        buf[i] = c >= 'a' && c <= 'z' ? (Byte)(c - 'a' + 'A') : c;
    }
    return jsontext_encoder_write_token(e, jsontext_string(str_from_bytes(buf, n)));
}

static Error upper_unmarshal_json_from(Upper *u, Alloc *a, Jsonv2DecoderArg d) {
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    u->S = strings_to_lower(a, jsontext_token_string(tok, a));
    return BURROW_NO_ERROR;
}

#define UPPER_METHODS(M, T)                                                            \
    M(T, MarshalJSONTo, upper_marshal_json_to, JSONV2_SIG_MARSHAL_JSON_TO)             \
    M(T, UnmarshalJSONFrom, upper_unmarshal_json_from, JSONV2_SIG_UNMARSHAL_JSON_FROM)
BURROW_STRUCT_DEFINE_METHODS(Upper, UPPER_FIELDS, UPPER_METHODS);

/* Every method at once, to see which one wins. */
#define ALL_FIELDS(F, T) F(T, Str, S, "")
BURROW_STRUCT_DECL(All, ALL_FIELDS);

static Slice all_marshal_json(All *x, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_of(a, slice_nil(TYPE_BYTE), "\"json\"");
}

static Error all_marshal_json_to(All *x, Jsonv2EncoderArg e) {
    return jsontext_encoder_write_token(e, jsontext_string(BURROW_S("to")));
}

static Slice all_marshal_text(All *x, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_of(a, slice_nil(TYPE_BYTE), "text");
}

static Error all_unmarshal_json(All *x, Alloc *a, Slice b) {
    x->S = BURROW_S("json");
    return BURROW_NO_ERROR;
}

static Error all_unmarshal_json_from(All *x, Alloc *a, Jsonv2DecoderArg d) {
    Error err = BURROW_NO_ERROR;
    (void)jsontext_decoder_read_value(d, &err);
    x->S = BURROW_S("from");
    return err;
}

static Error all_unmarshal_text(All *x, Alloc *a, Slice b) {
    x->S = BURROW_S("text");
    return BURROW_NO_ERROR;
}

#define ALL_METHODS(M, T)                                                              \
    M(T, MarshalJSON, all_marshal_json, JSONV2_SIG_MARSHAL_JSON)                       \
    M(T, MarshalJSONTo, all_marshal_json_to, JSONV2_SIG_MARSHAL_JSON_TO)               \
    M(T, MarshalText, all_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                     \
    M(T, UnmarshalJSON, all_unmarshal_json, JSONV2_SIG_UNMARSHAL_JSON)                 \
    M(T, UnmarshalJSONFrom, all_unmarshal_json_from, JSONV2_SIG_UNMARSHAL_JSON_FROM)   \
    M(T, UnmarshalText, all_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(All, ALL_FIELDS, ALL_METHODS);

/* A MarshalJSONTo that steps aside for MarshalJSON. */
#define EMPTY_FIELDS(F, T) F(T, Int, unused, "json:\"-\"")

BURROW_STRUCT_DECL(Skip, EMPTY_FIELDS);

static Error skip_marshal_json_to(Skip *s, Jsonv2EncoderArg e) {
    return errors_err_unsupported;
}

static Slice skip_marshal_json(Skip *s, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_of(a, slice_nil(TYPE_BYTE), "\"fell through\"");
}

#define SKIP_METHODS(M, T)                                                             \
    M(T, MarshalJSON, skip_marshal_json, JSONV2_SIG_MARSHAL_JSON)                      \
    M(T, MarshalJSONTo, skip_marshal_json_to, JSONV2_SIG_MARSHAL_JSON_TO)
BURROW_STRUCT_DEFINE_METHODS(Skip, EMPTY_FIELDS, SKIP_METHODS);

/* A MarshalJSONTo that writes and then says it is unsupported. */
BURROW_STRUCT_DECL(Mutate, EMPTY_FIELDS);

static Error mutate_marshal_json_to(Mutate *s, Jsonv2EncoderArg e) {
    (void)jsontext_encoder_write_token(e, jsontext_string(BURROW_S("x")));
    return errors_err_unsupported;
}

#define MUTATE_METHODS(M, T)                                                           \
    M(T, MarshalJSONTo, mutate_marshal_json_to, JSONV2_SIG_MARSHAL_JSON_TO)
BURROW_STRUCT_DEFINE_METHODS(Mutate, EMPTY_FIELDS, MUTATE_METHODS);

/* A MarshalJSONTo that writes nothing. */
BURROW_STRUCT_DECL(Nothing, EMPTY_FIELDS);

static Error nothing_marshal_json_to(Nothing *s, Jsonv2EncoderArg e) {
    return BURROW_NO_ERROR;
}

#define NOTHING_METHODS(M, T)                                                          \
    M(T, MarshalJSONTo, nothing_marshal_json_to, JSONV2_SIG_MARSHAL_JSON_TO)
BURROW_STRUCT_DEFINE_METHODS(Nothing, EMPTY_FIELDS, NOTHING_METHODS);

/* A MarshalJSON that returns half an object. */
BURROW_STRUCT_DECL(Bad, EMPTY_FIELDS);

static Slice bad_marshal_json(Bad *s, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_of(a, slice_nil(TYPE_BYTE), "{\"a\":");
}

#define BAD_METHODS(M, T) M(T, MarshalJSON, bad_marshal_json, JSONV2_SIG_MARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(Bad, EMPTY_FIELDS, BAD_METHODS);

/* Methods that fail. */
BURROW_STRUCT_DECL(Fails, EMPTY_FIELDS);

static Slice fails_marshal_text(Fails *s, Alloc *a, Error *err) {
    BURROW_OUT(err, boom());
    return slice_nil(TYPE_BYTE);
}

static Error fails_unmarshal_json(Fails *s, Alloc *a, Slice b) {
    return boom();
}

#define FAILS_METHODS(M, T)                                                            \
    M(T, MarshalText, fails_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                   \
    M(T, UnmarshalJSON, fails_unmarshal_json, JSONV2_SIG_UNMARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(Fails, EMPTY_FIELDS, FAILS_METHODS);

/* A named string whose text is upper case, as a map key. */
typedef Str Key;

static Slice key_marshal_text(Key *k, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = strings_to_upper(a, *k);
    return slice_append(a, slice_nil(TYPE_BYTE), s.p, s.len);
}

static Error key_unmarshal_text(Key *k, Alloc *a, Slice b) {
    *k = strings_to_lower(a, str_from_bytes(b.p, b.len));
    return BURROW_NO_ERROR;
}

#define KEY_METHODS(M, T)                                                              \
    M(T, MarshalText, key_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                     \
    M(T, UnmarshalText, key_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)
BURROW_METHODS_DEFINE(Key, KEY_METHODS);

/* Not const, since a named string borrows the ops of string, which the test
 * copies in before it runs, as the generated tests do. */
static Type burrow_type_Key = {
    BURROW_S_INIT("Key"),
    {NULL, 0},
    KIND_STRING,
    (uint32_t)sizeof(Key),
    _Alignof(Key),
    0,
    (uint16_t)(sizeof burrow__methods_Key / sizeof burrow__methods_Key[0]),
    NULL,
    burrow__methods_Key,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Type map_key_int = {
    BURROW_S_INIT(""),
    {NULL, 0},
    KIND_MAP,
    (uint32_t)sizeof(Map *),
    _Alignof(Map *),
    0,
    0,
    NULL,
    NULL,
    TYPE_INT,
    &burrow_type_Key,
    0,
    0,
    NULL,
};

/* IsZero, true for a negative N. */
#define MAYBE_FIELDS(F, T) F(T, Int, N, "")
BURROW_STRUCT_DECL(Maybe, MAYBE_FIELDS);

static bool maybe_is_zero(Maybe *m) {
    return m->N < 0;
}

#define MAYBE_METHODS(M, T) M(T, IsZero, maybe_is_zero, JSONV2_SIG_IS_ZERO)
BURROW_STRUCT_DEFINE_METHODS(Maybe, MAYBE_FIELDS, MAYBE_METHODS);

typedef Maybe *MaybePtr;
static const Type burrow_type_MaybePtr = {
    BURROW_S_INIT(""),
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(MaybePtr),
    _Alignof(MaybePtr),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Maybe,
    NULL,
    0,
    0,
    NULL,
};

/* The methods on fields. */
#define HOLDER_FIELDS(F, T)                                                            \
    F(T, Celsius, C, "")                                                               \
    F(T, PointPtr, P, "")                                                              \
    F(T, PointPtr, Q, "")                                                              \
    F(T, Maybe, M, "json:\",omitzero\"")                                               \
    F(T, Maybe, M2, "json:\",omitzero\"")                                              \
    F(T, MaybePtr, MP, "json:\",omitzero\"")                                           \
    F(T, Upper, U, "json:\",omitempty\"")
BURROW_STRUCT_DECL(Holder, HOLDER_FIELDS);
BURROW_STRUCT_DEFINE(Holder, HOLDER_FIELDS);

/* ------------------------------------------------------------------ tests */

static Str q(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static void check_marshal(TestingT *t, const char *name, Any v, JsontextOptions *o,
                          const char *want, const char *want_err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice opts = o == NULL ? slice_nil(TYPE_JSONTEXT_OPTIONS)
                           : (Slice){o, 1, 1, TYPE_JSONTEXT_OPTIONS};
    Slice out = jsonv2_marshal(a, v, opts, &err);
    Str got_err = BURROW_FAILED(err) ? error_text(err) : BURROW_S("");
    if (!str_eq(q(out), str_from_bytes(want, (Int)strlen(want))))
        testing_t_errorf_v(t, "%s: Marshal = %q, want %q", name, q(out), want);
    if (!str_eq(got_err, str_from_bytes(want_err, (Int)strlen(want_err))))
        testing_t_errorf_v(t, "%s: Marshal error = %q, want %q", name, got_err,
                           want_err);
    arena_free(&ar);
}

static Error unmarshal(Alloc *a, const char *in, Any v) {
    return jsonv2_unmarshal(a, bytes_of(a, slice_nil(TYPE_BYTE), in), v,
                            slice_nil(TYPE_JSONTEXT_OPTIONS));
}

static void check_err(TestingT *t, const char *name, Error err, const char *want) {
    Str got = BURROW_FAILED(err) ? error_text(err) : BURROW_S("");
    if (!str_eq(got, str_from_bytes(want, (Int)strlen(want))))
        testing_t_errorf_v(t, "%s: error = %q, want %q", name, got, want);
}

static void TestMarshalMethods(TestingT *t) {
    Celsius c = {21.5};
    check_marshal(t, "MarshalJSON", BURROW_ANY(TYPE_OF(Celsius), &c), NULL,
                  "{\"c\":21.5}", "");
    Point p = {3, 4};
    check_marshal(t, "MarshalText", BURROW_ANY(TYPE_OF(Point), &p), NULL, "\"3,4\"",
                  "");
    Hex h = {255};
    check_marshal(t, "AppendText", BURROW_ANY(TYPE_OF(Hex), &h), NULL, "\"0xff\"", "");
    Upper u = {BURROW_S("go")};
    check_marshal(t, "MarshalJSONTo", BURROW_ANY(TYPE_OF(Upper), &u), NULL, "\"GO\"",
                  "");
    All all = {BURROW_S("")};
    check_marshal(t, "precedence", BURROW_ANY(TYPE_OF(All), &all), NULL, "\"to\"", "");
    Skip s = {0};
    check_marshal(t, "ErrUnsupported", BURROW_ANY(TYPE_OF(Skip), &s), NULL,
                  "\"fell through\"", "");
}

static void TestMarshalMethodErrors(TestingT *t) {
    Mutate m = {0};
    check_marshal(t, "mutation", BURROW_ANY(TYPE_OF(Mutate), &m), NULL, "\"x\"",
                  "json: cannot marshal from Go Mutate after offset 3: unsupported "
                  "calls must not read or write any tokens");
    Nothing n = {0};
    check_marshal(t, "nothing", BURROW_ANY(TYPE_OF(Nothing), &n), NULL, "",
                  "json: cannot marshal from Go Nothing: must read or write exactly "
                  "one value");
    Bad b = {0};
    check_marshal(t, "invalid", BURROW_ANY(TYPE_OF(Bad), &b), NULL, "",
                  "json: cannot marshal from Go Bad: unexpected EOF within \"/a\" "
                  "after offset 5");
    Fails f = {0};
    check_marshal(t, "text error", BURROW_ANY(TYPE_OF(Fails), &f), NULL, "",
                  "json: cannot marshal from Go Fails: boom");
}

static void TestMapKeyMethods(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow_type_Key.ops = TYPE_STRING->ops;
    Map *m = map_make(a, &burrow_type_Key, TYPE_INT, 0);
    Key k1 = BURROW_S("b"), k2 = BURROW_S("a");
    Int v1 = 2, v2 = 1;
    map_set(m, &k1, &v1);
    map_set(m, &k2, &v2);
    JsontextOptions o = jsonv2_deterministic(true);
    check_marshal(t, "map key", BURROW_ANY(&map_key_int, &m), &o, "{\"A\":1,\"B\":2}",
                  "");

    Map *back = NULL;
    Error err = unmarshal(a, "{\"A\":1,\"B\":2}", BURROW_ANY(&map_key_int, &back));
    check_err(t, "map key", err, "");
    Key ka = BURROW_S("a"), kb = BURROW_S("b");
    const Int *ga = back == NULL ? NULL : (const Int *)map_get(back, &ka);
    const Int *gb = back == NULL ? NULL : (const Int *)map_get(back, &kb);
    if (ga == NULL || gb == NULL || *ga != 1 || *gb != 2)
        testing_t_error_v(t,
                          ("map key: UnmarshalText did not make the keys lower case"));
    arena_free(&ar);
}

static void TestFieldMethods(TestingT *t) {
    Point pt = {1, 2};
    Maybe mp = {-5};
    Holder h;
    memset(&h, 0, sizeof(h));
    h.C.Deg = 1;
    h.P = &pt;
    h.M.N = -1;
    h.MP = &mp;
    check_marshal(t, "fields", BURROW_ANY(TYPE_OF(Holder), &h), NULL,
                  "{\"C\":{\"c\":1},\"P\":\"1,2\",\"Q\":null,\"M2\":{\"N\":0}}", "");
    memset(&h, 0, sizeof(h));
    check_marshal(t, "empty fields", BURROW_ANY(TYPE_OF(Holder), &h), NULL,
                  "{\"C\":{\"c\":0},\"P\":null,\"Q\":null,\"M\":{\"N\":0},\"M2\":{"
                  "\"N\":0}}",
                  "");
}

static void TestUnmarshalMethods(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Celsius c = {0};
    check_err(t, "UnmarshalJSON",
              unmarshal(a, "{\"c\":30}", BURROW_ANY(TYPE_OF(Celsius), &c)), "");
    CHECK(c.Deg == 30);

    Point p = {0, 0};
    check_err(t, "UnmarshalText",
              unmarshal(a, "\"5,6\"", BURROW_ANY(TYPE_OF(Point), &p)), "");
    CHECK(p.X == 5 && p.Y == 6);
    check_err(t, "null", unmarshal(a, "null", BURROW_ANY(TYPE_OF(Point), &p)), "");
    CHECK(p.X == 0 && p.Y == 0);
    check_err(t, "number", unmarshal(a, "12", BURROW_ANY(TYPE_OF(Point), &p)),
              "json: cannot unmarshal JSON number into Go Point: JSON value must be "
              "string type");
    check_err(t, "text error",
              unmarshal(a, "\"nocomma\"", BURROW_ANY(TYPE_OF(Point), &p)),
              "json: cannot unmarshal JSON string into Go Point: no comma");

    Upper u = {BURROW_S("")};
    check_err(t, "UnmarshalJSONFrom",
              unmarshal(a, "\"GO\"", BURROW_ANY(TYPE_OF(Upper), &u)), "");
    CHECK(str_eq(u.S, BURROW_S("go")));

    All all = {BURROW_S("")};
    check_err(t, "precedence", unmarshal(a, "1", BURROW_ANY(TYPE_OF(All), &all)), "");
    CHECK(str_eq(all.S, BURROW_S("from")));

    Fails f = {0};
    check_err(t, "json error",
              unmarshal(a, "{\"x\":[1]}", BURROW_ANY(TYPE_OF(Fails), &f)),
              "json: cannot unmarshal JSON object into Go Fails: boom");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestMarshalMethods)                                                              \
    X(TestMarshalMethodErrors)                                                         \
    X(TestMapKeyMethods)                                                               \
    X(TestFieldMethods)                                                                \
    X(TestUnmarshalMethods)

TESTING_MAIN(TESTS)
