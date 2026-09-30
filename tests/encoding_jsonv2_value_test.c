/* jsontext.Value as a struct field, as a slice element and map value, and as
 * the embedded fallback that collects unknown members.
 *
 * Go's TestMarshal and TestUnmarshal cover most of this through the generated
 * cases. These are the ones they leave out: errors from a field's text, and
 * the fallback written with a nil pointer, with duplicate names allowed and
 * next to a second fallback. Every expected output and error below is what
 * go1.27.1 printed for the same program written in Go, with the "main." in
 * the type names dropped.
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

#include <string.h>

/* ------------------------------------------------------------------ types */

BURROW_PTR_TYPE(ValuePtr, JsontextValue);
BURROW_SLICE_TYPE(ValueSlice, JsontextValue);
BURROW_MAP_TYPE(StrValueMap, Str, JsontextValue);
BURROW_MAP_TYPE(StrStrMap, Str, Str);

#define REC_FIELDS(F, T)                                                               \
    F(T, JsontextValue, A, "json:\"a\"")                                               \
    F(T, JsontextValue, B, "json:\"b,omitempty\"")                                     \
    F(T, JsontextValue, C, "json:\"c,omitzero\"")                                      \
    F(T, ValuePtr, P, "json:\"p,omitempty\"")
BURROW_STRUCT(Rec, REC_FIELDS);

#define REST_FIELDS(F, T)                                                              \
    F(T, Str, Name, "")                                                                \
    F(T, JsontextValue, X, "json:\",embed\"")
BURROW_STRUCT(Rest, REST_FIELDS);

#define RESTP_FIELDS(F, T)                                                             \
    F(T, Str, Name, "")                                                                \
    F(T, ValuePtr, X, "json:\",embed\"")
BURROW_STRUCT(RestP, RESTP_FIELDS);

#define BOTH_FIELDS(F, T)                                                              \
    F(T, JsontextValue, X, "json:\",embed\"")                                          \
    F(T, StrStrMap, Y, "json:\",embed\"")
BURROW_STRUCT(Both, BOTH_FIELDS);

/* ---------------------------------------------------------------- helpers */

/* A value holding s, copied into a so it can be appended to. An empty s is an
 * empty value rather than a nil one, which is jsontext.Value("") in Go. */
static JsontextValue val(Alloc *a, const char *s) {
    Int n = (Int)strlen(s);
    if (n == 0)
        return (JsontextValue){(void *)(uintptr_t)"", 0, 0, TYPE_BYTE};
    return slice_append(a, slice_nil(TYPE_BYTE), s, n);
}

static Str q(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static Str cs(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

static void check_marshal(TestingT *t, const char *name, Alloc *a, Any v,
                          JsontextOptions o, const char *want, const char *want_err) {
    Error err = BURROW_NO_ERROR;
    Slice out = jsonv2_marshal(a, v, (Slice){&o, 1, 1, TYPE_JSONTEXT_OPTIONS}, &err);
    Str got_err = BURROW_FAILED(err) ? error_text(err) : BURROW_S("");
    if (!str_eq(q(out), cs(want)))
        testing_t_errorf_v(t, "%s: Marshal = %q, want %q", name, q(out), want);
    if (!str_eq(got_err, cs(want_err)))
        testing_t_errorf_v(t, "%s: Marshal error = %q, want %q", name, got_err,
                           want_err);
}

static Error unmarshal(Alloc *a, const char *in, Any v, JsontextOptions o) {
    return jsonv2_unmarshal(a, val(a, in), v, (Slice){&o, 1, 1, TYPE_JSONTEXT_OPTIONS});
}

static void check_err(TestingT *t, const char *name, Error err, const char *want) {
    Str got_err = BURROW_FAILED(err) ? error_text(err) : BURROW_S("");
    if (!str_eq(got_err, cs(want)))
        testing_t_errorf_v(t, "%s: error = %q, want %q", name, got_err, want);
}

static void check_val(TestingT *t, const char *name, JsontextValue v,
                      const char *want) {
    if (!str_eq(q(v), cs(want)))
        testing_t_errorf_v(t, "%s: value = %q, want %q", name, q(v), want);
}

/* ------------------------------------------------------------------ tests */

static void TestMarshalValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    JsontextOptions none;
    memset(&none, 0, sizeof(none));

    const struct {
        const char *name, *a, *b, *c, *want, *err;
    } recs[] = {
        {"compact", " [1,  2] ", NULL, NULL, "{\"a\":[1,2]}", ""},
        {"truncated", "{", NULL, NULL, "{\"a\"",
         "json: cannot marshal from Go jsontext.Value: unexpected EOF within \"/a\" "
         "after offset 6"},
        {"empty", "", NULL, NULL, "{\"a\"",
         "json: cannot marshal from Go jsontext.Value: unexpected EOF within \"/a\" "
         "after offset 5"},
        {"omit", "1", "{}", "", "{\"a\":1,\"c\"",
         "json: cannot marshal from Go jsontext.Value: unexpected EOF within \"/c\" "
         "after offset 11"},
        {"omitempty", "\"x\"", " \"\" ", NULL, "{\"a\":\"x\"}", ""},
        {"two values", "1 2", NULL, NULL, "{\"a\"",
         "json: cannot marshal from Go jsontext.Value: invalid character '2' after "
         "top-level value within \"/a\" after offset 7"},
        {"duplicate", "{\"b\":1,\"b\":2}", NULL, NULL, "{\"a\"",
         "json: cannot marshal from Go jsontext.Value: duplicate object member name "
         "\"b\" within \"/a\""},
        {"utf8", "\"\xff\"", NULL, NULL, "{\"a\"",
         "json: cannot marshal from Go jsontext.Value: invalid UTF-8 within \"/a\" "
         "after offset 6"},
    };
    for (size_t i = 0; i < sizeof(recs) / sizeof(recs[0]); i++) {
        Rec r;
        memset(&r, 0, sizeof(r));
        r.A = val(a, recs[i].a);
        if (recs[i].b != NULL)
            r.B = val(a, recs[i].b);
        if (recs[i].c != NULL)
            r.C = val(a, recs[i].c);
        check_marshal(t, recs[i].name, a, BURROW_ANY(TYPE_OF(Rec), &r), none,
                      recs[i].want, recs[i].err);
    }

    JsontextValue vs[3] = {
        val(a, "true"), {NULL, 0, 0, TYPE_BYTE}, val(a, "{\"k\" : [ ] }")};
    ValueSlice s = {vs, 3, 3, TYPE_OF(JsontextValue)};
    check_marshal(t, "slice", a, BURROW_ANY(TYPE_OF(ValueSlice), &s), none,
                  "[true,null,{\"k\":[]}]", "");

    StrValueMap m = map_make(a, TYPE_OF(Str), TYPE_OF(JsontextValue), 0);
    Str z = BURROW_S("z");
    JsontextValue null = val(a, "null");
    map_set(m, &z, &null);
    check_marshal(t, "map", a, BURROW_ANY(TYPE_OF(StrValueMap), &m), none,
                  "{\"z\":null}", "");
    arena_free(&ar);
}

static void TestMarshalFallbackValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    JsontextOptions none;
    memset(&none, 0, sizeof(none));
    JsontextOptions dup = jsontext_allow_duplicate_names(true);

    const struct {
        const char *name, *x;
        bool dup;
        const char *want, *err;
    } rests[] = {
        {"members", " { \"k\" : 1 , \"z\":[true] } ", false,
         "{\"Name\":\"n\",\"k\":1,\"z\":[true]}", ""},
        {"field name", "{\"Name\":2}", false, "{\"Name\":\"n\"",
         "jsontext: duplicate object member name \"Name\""},
        {"array", "[1]", false, "{\"Name\":\"n\"",
         "json: cannot marshal from Go jsontext.Value after offset 12: embedded raw "
         "value must be a JSON object"},
        {"empty", "", false, "{\"Name\":\"n\"}", ""},
        {"truncated", "{\"a\":1", false, "{\"Name\":\"n\",\"a\":1",
         "json: cannot marshal from Go jsontext.Value after offset 18: unexpected EOF "
         "after offset 6"},
        {"trailing", "{\"a\":1} x", false, "{\"Name\":\"n\",\"a\":1",
         "json: cannot marshal from Go jsontext.Value after offset 18: invalid "
         "character 'x' after top-level value after offset 8"},
        {"duplicate", "{\"a\":1,\"a\":2}", false, "{\"Name\":\"n\",\"a\":1",
         "jsontext: duplicate object member name \"a\""},
        {"duplicate allowed", "{\"a\":1,\"a\":2}", true,
         "{\"Name\":\"n\",\"a\":1,\"a\":2}", ""},
        {"field name allowed", "{\"Name\":2}", true, "{\"Name\":\"n\",\"Name\":2}", ""},
    };
    for (size_t i = 0; i < sizeof(rests) / sizeof(rests[0]); i++) {
        Rest r = {BURROW_S("n"), val(a, rests[i].x)};
        check_marshal(t, rests[i].name, a, BURROW_ANY(TYPE_OF(Rest), &r),
                      rests[i].dup ? dup : none, rests[i].want, rests[i].err);
    }

    RestP rp = {BURROW_S("n"), NULL};
    check_marshal(t, "nil pointer", a, BURROW_ANY(TYPE_OF(RestP), &rp), none,
                  "{\"Name\":\"n\"}", "");
    Both both;
    memset(&both, 0, sizeof(both));
    check_marshal(
        t, "both", a, BURROW_ANY(TYPE_OF(Both), &both), none, "",
        "json: cannot marshal from Go Both: embedded Go struct fields X and Y "
        "cannot both be a Go map or jsontext.Value");
    arena_free(&ar);
}

static void TestUnmarshalValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    JsontextOptions none;
    memset(&none, 0, sizeof(none));

    Rec r;
    memset(&r, 0, sizeof(r));
    check_err(t, "rec",
              unmarshal(a,
                        "{\"a\": [1,  2] , \"b\":null, \"c\":\"s\", \"p\": {\"q\":1}}",
                        BURROW_ANY(TYPE_OF(Rec), &r), none),
              "");
    check_val(t, "rec a", r.A, "[1,  2]");
    check_val(t, "rec b", r.B, "null");
    check_val(t, "rec c", r.C, "\"s\"");
    CHECK(r.P != NULL);
    if (r.P != NULL)
        check_val(t, "rec p", *r.P, "{\"q\":1}");
    check_err(t, "again", unmarshal(a, "{\"a\":1}", BURROW_ANY(TYPE_OF(Rec), &r), none),
              "");
    check_val(t, "again a", r.A, "1");

    ValueSlice vs = {NULL, 0, 0, TYPE_OF(JsontextValue)};
    check_err(t, "slice",
              unmarshal(a, "[1, {\"a\" : 2}, null]",
                        BURROW_ANY(TYPE_OF(ValueSlice), &vs), none),
              "");
    CHECK(vs.len == 3);
    if (vs.len == 3) {
        const JsontextValue *e = (const JsontextValue *)vs.p;
        check_val(t, "slice 0", e[0], "1");
        check_val(t, "slice 1", e[1], "{\"a\" : 2}");
        check_val(t, "slice 2", e[2], "null");
    }
    arena_free(&ar);
}

static void TestUnmarshalFallbackValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    JsontextOptions none;
    memset(&none, 0, sizeof(none));

    Rest rs;
    memset(&rs, 0, sizeof(rs));
    check_err(t, "collect",
              unmarshal(a, "{\"Name\":\"x\",\"k\":1, \"z\" : [true],\"Name2\":{}}",
                        BURROW_ANY(TYPE_OF(Rest), &rs), none),
              "");
    CHECK(str_eq(rs.Name, BURROW_S("x")));
    check_val(t, "collect", rs.X, "{\"k\":1,\"z\":[true],\"Name2\":{}}");

    check_err(t, "merge",
              unmarshal(a, "{\"w\":\"A\"}", BURROW_ANY(TYPE_OF(Rest), &rs), none), "");
    check_val(t, "merge", rs.X, "{\"k\":1,\"z\":[true],\"Name2\":{},\"w\":\"A\"}");

    rs.X = val(a, "{ } ");
    check_err(t, "empty object",
              unmarshal(a, "{\"q\":2}", BURROW_ANY(TYPE_OF(Rest), &rs), none), "");
    check_val(t, "empty object", rs.X, "{\"q\":2}");

    rs.X = val(a, "[1]");
    check_err(
        t, "array",
        unmarshal(a, "{\"q\":2,\"Name\":\"y\"}", BURROW_ANY(TYPE_OF(Rest), &rs), none),
        "json: cannot unmarshal JSON string into Go jsontext.Value within \"/q\": "
        "embedded raw value must be a JSON object");
    CHECK(str_eq(rs.Name, BURROW_S("x")));
    check_val(t, "array", rs.X, "[1]");

    rs.X = (JsontextValue){NULL, 0, 0, TYPE_BYTE};
    check_err(t, "duplicate",
              unmarshal(a, "{\"q\":2,\"q\":3}", BURROW_ANY(TYPE_OF(Rest), &rs), none),
              "jsontext: duplicate object member name \"q\"");
    check_val(t, "duplicate", rs.X, "{\"q\":2}");

    RestP rp;
    memset(&rp, 0, sizeof(rp));
    check_err(t, "pointer",
              unmarshal(a, "{\"a\":1}", BURROW_ANY(TYPE_OF(RestP), &rp), none), "");
    CHECK(rp.X != NULL);
    if (rp.X != NULL)
        check_val(t, "pointer", *rp.X, "{\"a\":1}");
    check_err(t, "reject unknown",
              unmarshal(a, "{\"a\":1}", BURROW_ANY(TYPE_OF(RestP), &rp),
                        jsonv2_reject_unknown_members(true)),
              "");
    if (rp.X != NULL)
        check_val(t, "reject unknown", *rp.X, "{\"a\":1,\"a\":1}");

    Both both;
    memset(&both, 0, sizeof(both));
    check_err(t, "both",
              unmarshal(a, "{\"a\":\"1\"}", BURROW_ANY(TYPE_OF(Both), &both), none),
              "json: cannot unmarshal JSON object into Go Both: embedded Go struct "
              "fields X and Y cannot both be a Go map or jsontext.Value");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestMarshalValue)                                                                \
    X(TestMarshalFallbackValue)                                                        \
    X(TestUnmarshalValue)                                                              \
    X(TestUnmarshalFallbackValue)

TESTING_MAIN(TESTS)
