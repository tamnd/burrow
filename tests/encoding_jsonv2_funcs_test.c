/* WithMarshalers and WithUnmarshalers: functions set up by the caller that
 * take over for a type, from MarshalFunc, MarshalToFunc, UnmarshalFunc and
 * UnmarshalFromFunc.
 *
 * Written by hand, since Go's own tests for these are built on generics the
 * test generator cannot describe. Every expected output and error below is
 * what go1.27.1 printed for the same program written in Go, with the "main."
 * in the type names dropped. Go picks "cannot" or "unable to" in its errors
 * at random, once per process, and burrow always says "cannot".
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

/* ------------------------------------------------------------------ types */

#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "json:\"x\"")                                                         \
    F(T, Int, Y, "json:\"y\"")
BURROW_STRUCT(Point, POINT_FIELDS);

BURROW_PTR_TYPE(PointPtr, Point);
BURROW_PTR_TYPE(BoolPtr, bool);
BURROW_PTR_TYPE(IntPtr, Int);
BURROW_PTR_TYPE(StrPtr, Str);
BURROW_SLICE_TYPE(IntSlice, Int);
BURROW_SLICE_TYPE(BoolSlice, bool);
BURROW_MAP_TYPE(StrIntMap, Str, Int);

/* A named string, not const since it borrows the ops of string, which the
 * tests copy in before they run. */
typedef Str Key;
static Type burrow_type_Key = {
    BURROW_S_INIT("Key"),
    {NULL, 0},
    KIND_STRING,
    (uint32_t)sizeof(Key),
    _Alignof(Key),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};
BURROW_PTR_TYPE(KeyPtr, Key);
BURROW_MAP_TYPE(KeyIntMap, Key, Int);

#define REC_FIELDS(F, T)                                                               \
    F(T, bool, On, "json:\"on\"")                                                      \
    F(T, Point, P, "json:\"p\"")                                                       \
    F(T, PointPtr, PP, "json:\"pp\"")                                                  \
    F(T, Str, Name, "json:\"name,omitempty\"")                                         \
    F(T, KeyIntMap, Tags, "json:\"tags\"")                                             \
    F(T, Any, Any, "json:\"any\"")                                                     \
    F(T, IntSlice, Nums, "json:\"nums\"")
BURROW_STRUCT(Rec, REC_FIELDS);

#define PHOLDER_FIELDS(F, T) F(T, Point, P, "")
BURROW_STRUCT(PHolder, PHOLDER_FIELDS);

/* MarshalText, to be found through encoding.TextMarshaler. */
#define TXT_FIELDS(F, T) F(T, Int, N, "")
BURROW_STRUCT_DECL(Txt, TXT_FIELDS);

static Slice txt_marshal_text(Txt *x, Alloc *a, Error *err) {
    (void)x;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return slice_append(a, slice_nil(TYPE_BYTE), "txt", 3);
}

#define TXT_METHODS(M, T) M(T, MarshalText, txt_marshal_text, ENCODING_SIG_MARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Txt, TXT_FIELDS, TXT_METHODS);

BURROW_PTR_TYPE(TxtPtr, Txt);

/* -------------------------------------------------------------- functions */

/* Contexts for word and nope, writable since a ctx is a void *. */
static char w_first[] = "\"first\"";
static char w_second[] = "\"second\"";
static char w_iface[] = "\"iface\"";
static char w_brace[] = "{";
static char w_no[] = "no";

static Slice lit(Alloc *a, const char *s) {
    return slice_append(a, slice_nil(TYPE_BYTE), s, (Int)strlen(s));
}

static Slice str_bytes(Str s) {
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

static Error fail(const char *msg) {
    return errors_new(error_allocator(), str_from_bytes(msg, (Int)strlen(msg)));
}

static Slice yes_no(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return lit(a, *(const bool *)v.data ? "\"yes\"" : "\"no\"");
}

static Slice point_text(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    BURROW_OUT(err, BURROW_NO_ERROR);
    const Point *p = (const Point *)v.data;
    return str_bytes(fmt_sprintf_v(a, "\"%d,%d\"", p->X, p->Y));
}

static Error evens(void *ctx, JsontextEncoder *e, Any v) {
    Alloc *a = (Alloc *)ctx;
    Int n = *(const Int *)v.data;
    if (n % 2 != 0)
        return errors_err_unsupported;
    return jsontext_encoder_write_token(
        e, jsontext_string(fmt_sprintf_v(a, "even %d", n)));
}

static Slice upper_key(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return str_bytes(strconv_quote(a, strings_to_upper(a, *(const Key *)v.data)));
}

static Slice hide_blank(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = *(const Str *)v.data;
    if (str_eq(s, BURROW_S("hide")))
        return lit(a, "\"\"");
    return str_bytes(strconv_quote(a, s));
}

static Error angle(void *ctx, JsontextEncoder *e, Any v) {
    Alloc *a = (Alloc *)ctx;
    Str s = *(const Str *)v.data;
    return jsontext_encoder_write_token(e,
                                        jsontext_string(fmt_sprintf_v(a, "<%s>", s)));
}

static Slice word(void *ctx, Alloc *a, Any v, Error *err) {
    (void)v;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return lit(a, (const char *)ctx);
}

static Slice no_ints(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    (void)a;
    (void)v;
    BURROW_OUT(err, fail("no ints"));
    return slice_nil(TYPE_BYTE);
}

static Slice unsupported(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    (void)a;
    (void)v;
    BURROW_OUT(err, errors_err_unsupported);
    return slice_nil(TYPE_BYTE);
}

static Slice nope(void *ctx, Alloc *a, Any v, Error *err) {
    (void)a;
    (void)v;
    BURROW_OUT(err, fail((const char *)ctx == NULL ? "nope" : (const char *)ctx));
    return slice_nil(TYPE_BYTE);
}

static Error two_values(void *ctx, JsontextEncoder *e, Any v) {
    (void)ctx;
    (void)v;
    (void)jsontext_encoder_write_token(e, jsontext_int(1));
    return jsontext_encoder_write_token(e, jsontext_int(2));
}

static Error mutate(void *ctx, JsontextEncoder *e, Any v) {
    (void)ctx;
    (void)v;
    (void)jsontext_encoder_write_token(e, jsontext_begin_array);
    return errors_err_unsupported;
}

static Error parse_yes_no(void *ctx, Alloc *a, Slice data, Any v) {
    (void)ctx;
    (void)a;
    Str s = str_from_bytes(data.p, data.len);
    if (str_eq(s, BURROW_S("\"yes\"")))
        *(bool *)v.data = true;
    else if (str_eq(s, BURROW_S("\"no\"")))
        *(bool *)v.data = false;
    else
        return fail("want yes or no");
    return BURROW_NO_ERROR;
}

static Error quoted_int(void *ctx, Alloc *a, JsontextDecoder *d, Any v) {
    (void)ctx;
    if (jsontext_decoder_peek_kind(d) != '"')
        return errors_err_unsupported;
    Error err = BURROW_NO_ERROR;
    JsontextToken tok = jsontext_decoder_read_token(d, &err);
    if (BURROW_FAILED(err))
        return err;
    Int n = strconv_atoi(jsontext_token_string(tok, a), &err);
    if (BURROW_FAILED(err))
        return err;
    *(Int *)v.data = n;
    return BURROW_NO_ERROR;
}

static Error lower_key(void *ctx, Alloc *a, Slice data, Any v) {
    (void)ctx;
    Str s = strings_trim(str_from_bytes(data.p, data.len), BURROW_S("\""));
    *(Key *)v.data = strings_to_lower(a, s);
    return BURROW_NO_ERROR;
}

static Error got(void *ctx, Alloc *a, Slice data, Any v) {
    (void)ctx;
    Str s = str_from_bytes(data.p, data.len);
    *(Str *)v.data = fmt_sprintf_v(a, "got %s", s);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ tests */

static Str q(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static void check_marshal(TestingT *t, const char *name, Alloc *a, Any v,
                          JsontextOptions o, const char *want, const char *want_err) {
    Error err = BURROW_NO_ERROR;
    Slice out = jsonv2_marshal(a, v, (Slice){&o, 1, 1, TYPE_JSONTEXT_OPTIONS}, &err);
    Str got_err = BURROW_FAILED(err) ? error_text(err) : BURROW_S("");
    if (!str_eq(q(out), str_from_bytes(want, (Int)strlen(want))))
        testing_t_errorf_v(t, "%s: Marshal = %q, want %q", name, q(out), want);
    if (!str_eq(got_err, str_from_bytes(want_err, (Int)strlen(want_err))))
        testing_t_errorf_v(t, "%s: Marshal error = %q, want %q", name, got_err,
                           want_err);
}

static Error unmarshal(Alloc *a, const char *in, Any v, JsontextOptions o) {
    return jsonv2_unmarshal(a, lit(a, in), v, (Slice){&o, 1, 1, TYPE_JSONTEXT_OPTIONS});
}

static void check_err(TestingT *t, const char *name, Error err, const char *want) {
    Str got_err = BURROW_FAILED(err) ? error_text(err) : BURROW_S("");
    if (!str_eq(got_err, str_from_bytes(want, (Int)strlen(want))))
        testing_t_errorf_v(t, "%s: error = %q, want %q", name, got_err, want);
}

static void TestMarshalFuncs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow_type_Key.ops = TYPE_STRING->ops;

    Jsonv2Marshalers *yn = jsonv2_marshal_func(a, TYPE_BOOL, yes_no, NULL);
    Jsonv2Marshalers *pt = jsonv2_marshal_func(a, TYPE_OF(PointPtr), point_text, NULL);
    Jsonv2Marshalers *ev = jsonv2_marshal_to_func(a, TYPE_INT, evens, a);
    Jsonv2Marshalers *keys = jsonv2_marshal_func(a, TYPE_OF(Key), upper_key, NULL);
    Jsonv2Marshalers *blank = jsonv2_marshal_func(a, TYPE_STRING, hide_blank, NULL);
    Jsonv2Marshalers *all = jsonv2_join_marshalers_v(a, 5, yn, pt, ev, keys, blank);

    Point pp = {3, 4};
    bool no = false;
    Int nums[] = {1, 2, 3};
    Rec r;
    memset(&r, 0, sizeof(r));
    r.On = true;
    r.P = (Point){1, 2};
    r.PP = &pp;
    r.Name = BURROW_S("hide");
    r.Tags = map_make(a, TYPE_OF(Key), TYPE_INT, 0);
    Key ka = BURROW_S("a");
    Int one = 1;
    map_set(r.Tags, &ka, &one);
    r.Any = BURROW_ANY(TYPE_BOOL, &no);
    r.Nums = (Slice){nums, 3, 3, TYPE_INT};
    JsontextOptions o = jsonv2_join_options_v(2, jsonv2_with_marshalers(all),
                                              jsonv2_deterministic(true));
    check_marshal(t, "rec", a, BURROW_ANY(TYPE_OF(Rec), &r), o,
                  "{\"on\":\"yes\",\"p\":\"1,2\",\"pp\":\"3,4\",\"tags\":{\"A\":1},"
                  "\"any\":\"no\",\"nums\":[1,\"even 2\",3]}",
                  "");
    check_marshal(
        t, "plain", a, BURROW_ANY(TYPE_OF(Rec), &r), jsonv2_deterministic(true),
        "{\"on\":true,\"p\":{\"x\":1,\"y\":2},\"pp\":{\"x\":3,\"y\":4},"
        "\"name\":\"hide\",\"tags\":{\"a\":1},\"any\":false,\"nums\":[1,2,3]}",
        "");

    /* A string function has to see the strings inside an any, which the fast
     * path would otherwise write itself. */
    Map *m = map_make(a, TYPE_STRING, TYPE_ANY, 0);
    Str x = BURROW_S("x"), y = BURROW_S("y");
    double f1 = 1;
    Any arr[] = {BURROW_ANY(TYPE_STRING, &y), BURROW_ANY(TYPE_FLOAT64, &f1)};
    Slice arrs = {arr, 2, 2, TYPE_ANY};
    Str kxa = BURROW_S("a"), kxb = BURROW_S("b");
    Any va = BURROW_ANY(TYPE_STRING, &x), vb = BURROW_ANY(TYPE_JSONV2_SLICE_ANY, &arrs);
    map_set(m, &kxa, &va);
    map_set(m, &kxb, &vb);
    Jsonv2Marshalers *an = jsonv2_marshal_to_func(a, TYPE_STRING, angle, a);
    check_marshal(t, "any", a, BURROW_ANY(TYPE_JSONV2_MAP_STRING_ANY, &m),
                  jsonv2_join_options_v(2, jsonv2_with_marshalers(an),
                                        jsonv2_deterministic(true)),
                  "{\"<a>\":\"<x>\",\"<b>\":[\"<y>\",1]}", "");

    Int two[] = {1, 2};
    Slice twos = {two, 2, 2, TYPE_INT};
    Jsonv2Marshalers *order = jsonv2_join_marshalers_v(
        a, 3, ev, jsonv2_marshal_func(a, TYPE_INT, word, w_first),
        jsonv2_marshal_func(a, TYPE_INT, word, w_second));
    check_marshal(t, "order", a, BURROW_ANY(TYPE_OF(IntSlice), &twos),
                  jsonv2_with_marshalers(order), "[\"first\",\"even 2\"]", "");

    /* An interface takes any value whose type has its methods, and a pointer
     * is followed to the value first. */
    Txt tx;
    Int n1 = 1;
    Txt *txp = &tx;
    Any items[] = {BURROW_ANY(TYPE_OF(Txt), &tx), BURROW_ANY(TYPE_INT, &n1),
                   BURROW_ANY(TYPE_OF(TxtPtr), &txp)};
    Slice itemss = {items, 3, 3, TYPE_ANY};
    Jsonv2Marshalers *tm =
        jsonv2_marshal_func(a, &burrow_type_EncodingTextMarshaler, word, w_iface);
    check_marshal(t, "interface", a, BURROW_ANY(TYPE_JSONV2_SLICE_ANY, &itemss),
                  jsonv2_with_marshalers(tm), "[\"iface\",1,\"iface\"]", "");

    const Jsonv2Marshalers *back = NULL;
    CHECK(jsonv2_get_marshalers(jsonv2_with_marshalers(tm), &back) && back == tm);
    CHECK(!jsonv2_get_marshalers(jsonv2_deterministic(true), &back) && back == NULL);
    CHECK(jsonv2_join_marshalers_v(a, 2, NULL, NULL) == NULL);
    arena_free(&ar);
}

static void TestMarshalFuncErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Str ka = BURROW_S("a");
    Int one = 1;
    map_set(m, &ka, &one);
    check_marshal(
        t, "error", a, BURROW_ANY(TYPE_OF(StrIntMap), &m),
        jsonv2_with_marshalers(jsonv2_marshal_func(a, TYPE_INT, no_ints, NULL)),
        "{\"a\"", "json: cannot marshal from Go int within \"/a\": no ints");

    Slice ones = {&one, 1, 1, TYPE_INT};
    check_marshal(
        t, "invalid", a, BURROW_ANY(TYPE_OF(IntSlice), &ones),
        jsonv2_with_marshalers(jsonv2_marshal_func(a, TYPE_INT, word, w_brace)), "[",
        "json: cannot marshal from Go int: unexpected EOF within \"/0\" after offset "
        "2");
    check_marshal(
        t, "ErrUnsupported", a, BURROW_ANY(TYPE_OF(IntSlice), &ones),
        jsonv2_with_marshalers(jsonv2_marshal_func(a, TYPE_INT, unsupported, NULL)),
        "[",
        "json: cannot marshal from Go int within \"/0\": marshal function of type "
        "func(T) ([]byte, error) may not return errors.ErrUnsupported");
    check_marshal(
        t, "two values", a, BURROW_ANY(TYPE_OF(IntSlice), &ones),
        jsonv2_with_marshalers(jsonv2_marshal_to_func(a, TYPE_INT, two_values, NULL)),
        "[1,2",
        "json: cannot marshal from Go int after offset 5: must read or write exactly "
        "one value");
    check_marshal(
        t, "mutation", a, BURROW_ANY(TYPE_OF(IntSlice), &ones),
        jsonv2_with_marshalers(jsonv2_marshal_to_func(a, TYPE_INT, mutate, NULL)), "[[",
        "json: cannot marshal from Go int within \"/0\": unsupported calls must not "
        "read or write any tokens");

    /* Errors name the type the function was set up for. */
    PHolder h;
    memset(&h, 0, sizeof(h));
    check_marshal(
        t, "pointer", a, BURROW_ANY(TYPE_OF(PHolder), &h),
        jsonv2_with_marshalers(jsonv2_marshal_func(a, TYPE_OF(PointPtr), nope, NULL)),
        "{\"P\"", "json: cannot marshal from Go *Point within \"/P\": nope");
    Txt tx;
    check_marshal(t, "interface", a, BURROW_ANY(TYPE_OF(Txt), &tx),
                  jsonv2_with_marshalers(jsonv2_marshal_func(
                      a, &burrow_type_EncodingTextMarshaler, nope, w_no)),
                  "", "json: cannot marshal from Go encoding.TextMarshaler: no");
    arena_free(&ar);
}

static void TestUnmarshalFuncs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow_type_Key.ops = TYPE_STRING->ops;

    Jsonv2Unmarshalers *yn =
        jsonv2_unmarshal_func(a, TYPE_OF(BoolPtr), parse_yes_no, NULL);
    Jsonv2Unmarshalers *qi =
        jsonv2_unmarshal_from_func(a, TYPE_OF(IntPtr), quoted_int, NULL);
    Jsonv2Unmarshalers *both = jsonv2_join_unmarshalers_v(a, 2, yn, qi);

    Rec r;
    memset(&r, 0, sizeof(r));
    check_err(
        t, "rec",
        unmarshal(a,
                  "{\"on\":\"yes\",\"nums\":[1,\"2\",3],\"p\":{\"x\":\"5\",\"y\":6}}",
                  BURROW_ANY(TYPE_OF(Rec), &r), jsonv2_with_unmarshalers(both)),
        "");
    CHECK(r.On);
    CHECK(r.P.X == 5 && r.P.Y == 6);
    CHECK(r.Nums.len == 3);
    if (r.Nums.len == 3) {
        const Int *ns = (const Int *)r.Nums.p;
        CHECK(ns[0] == 1 && ns[1] == 2 && ns[2] == 3);
    }

    Slice bs = slice_nil(TYPE_BOOL);
    check_err(
        t, "error",
        unmarshal(a, "[\"yes\",\"maybe\"]", BURROW_ANY(TYPE_OF(BoolSlice), &bs),
                  jsonv2_with_unmarshalers(yn)),
        "json: cannot unmarshal JSON string into Go *bool within \"/1\": want yes "
        "or no");
    CHECK(bs.len == 2 && ((const bool *)bs.p)[0] && !((const bool *)bs.p)[1]);

    Slice ns = slice_nil(TYPE_INT);
    check_err(
        t, "from error",
        unmarshal(a, "[1,\"x\"]", BURROW_ANY(TYPE_OF(IntSlice), &ns),
                  jsonv2_with_unmarshalers(qi)),
        "json: cannot unmarshal into Go *int within \"/1\": strconv.Atoi: parsing "
        "\"x\": invalid syntax");

    /* Keys that a function makes the same are duplicates. */
    Map *km = NULL;
    check_err(t, "keys",
              unmarshal(a, "{\"A\":1,\"a\":2}", BURROW_ANY(TYPE_OF(KeyIntMap), &km),
                        jsonv2_with_unmarshalers(jsonv2_unmarshal_func(
                            a, TYPE_OF(KeyPtr), lower_key, NULL))),
              "jsontext: duplicate object member name \"a\"");

    /* A string function sees the strings the any fast path would make. */
    Any v = {NULL, NULL};
    check_err(t, "any",
              unmarshal(a, "{\"a\":[\"x\",1]}", BURROW_ANY(TYPE_ANY, &v),
                        jsonv2_with_unmarshalers(
                            jsonv2_unmarshal_func(a, TYPE_OF(StrPtr), got, NULL))),
              "");
    Error err = BURROW_NO_ERROR;
    Slice out = jsonv2_marshal(a, v, slice_nil(TYPE_JSONTEXT_OPTIONS), &err);
    check_err(t, "any remarshal", err, "");
    if (!str_eq(q(out), BURROW_S("{\"got \\\"a\\\"\":[\"got \\\"x\\\"\",1]}")))
        testing_t_errorf_v(t, "any: got %q", q(out));

    const Jsonv2Unmarshalers *back = NULL;
    CHECK(jsonv2_get_unmarshalers(jsonv2_with_unmarshalers(yn), &back) && back == yn);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestMarshalFuncs)                                                                \
    X(TestMarshalFuncErrors)                                                           \
    X(TestUnmarshalFuncs)

TESTING_MAIN(TESTS)
