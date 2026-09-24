/* The encoding interfaces, found on a value and called.
 *
 * Go's encoding package has no tests, because it is six interface
 * declarations and the compiler does the rest. Here the rest is code, so this
 * checks it: a method in a descriptor is found when its signature is right and
 * not when it is wrong, a pointer finds the methods of what it points at, an
 * Any holding one of the interface values calls through the vtable, and a
 * value with no method gets Go's type assertion error.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"

#include "check.h"

#include <string.h>

/* A point that goes to text as "x,y" and to binary as two bytes. */
#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")

BURROW_STRUCT_DECL(Point, POINT_FIELDS);

static Slice point_append_text(Point *p, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "%d,%d", p->X, p->Y);
    return slice_append(a, b, s.p, s.len);
}

static Slice point_marshal_text(Point *p, Alloc *a, Error *err) {
    return point_append_text(p, a, slice_nil(TYPE_BYTE), err);
}

static Error point_unmarshal_text(Point *p, Alloc *a, Slice text) {
    (void)a;
    Str y;
    bool found;
    Str x = strings_cut(str_from_bytes(text.p, text.len), BURROW_S(","), &y, &found);
    if (!found)
        return errors_new(error_allocator(), BURROW_S("point: no comma"));
    Error err = BURROW_NO_ERROR;
    p->X = strconv_atoi(x, &err);
    if (BURROW_FAILED(err))
        return err;
    p->Y = strconv_atoi(y, &err);
    return err;
}

static Slice point_append_binary(Point *p, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Byte two[2] = {(Byte)p->X, (Byte)p->Y};
    return slice_append(a, b, two, 2);
}

static Slice point_marshal_binary(Point *p, Alloc *a, Error *err) {
    return point_append_binary(p, a, slice_nil(TYPE_BYTE), err);
}

static Error point_unmarshal_binary(Point *p, Alloc *a, Slice data) {
    (void)a;
    if (data.len != 2)
        return errors_new(error_allocator(), BURROW_S("point: want two bytes"));
    p->X = ((const Byte *)data.p)[0];
    p->Y = ((const Byte *)data.p)[1];
    return BURROW_NO_ERROR;
}

#define POINT_METHODS(M, T)                                                            \
    M(T, AppendBinary, point_append_binary, ENCODING_SIG_APPEND_BINARY)                \
    M(T, AppendText, point_append_text, ENCODING_SIG_APPEND_TEXT)                      \
    M(T, MarshalBinary, point_marshal_binary, ENCODING_SIG_MARSHAL_BINARY)             \
    M(T, MarshalText, point_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                   \
    M(T, UnmarshalBinary, point_unmarshal_binary, ENCODING_SIG_UNMARSHAL_BINARY)       \
    M(T, UnmarshalText, point_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);

/* A MarshalText with Go's name and the wrong signature, which does not make
 * the type a TextMarshaler. */
#define ODD_FIELDS(F, T) F(T, Int, N, "")

BURROW_STRUCT_DECL(Odd, ODD_FIELDS);

static Str odd_marshal_text(Odd *o) {
    (void)o;
    return BURROW_S("odd");
}

#define ODD_SIG_MarshalText(IN, OUT) OUT(Str)
#define ODD_METHODS(M, T) M(T, MarshalText, odd_marshal_text, ODD_SIG_MarshalText)

BURROW_STRUCT_DEFINE_METHODS(Odd, ODD_FIELDS, ODD_METHODS);

typedef Point *PointPtr;

static const Type burrow_type_PointPtr = {
    BURROW_S_INIT(""),
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(PointPtr),
    (uint16_t)_Alignof(PointPtr),
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

static Str q(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static void TestMarshalThroughDescriptor(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Point p = {3, 4};
    Any v = BURROW_ANY(TYPE_OF(Point), &p);

    CHECK(encoding_is_text_marshaler(v));
    CHECK(encoding_is_text_unmarshaler(v));
    CHECK(encoding_is_text_appender(v));
    CHECK(encoding_is_binary_marshaler(v));
    CHECK(encoding_is_binary_unmarshaler(v));
    CHECK(encoding_is_binary_appender(v));

    Error err = BURROW_NO_ERROR;
    Slice text = encoding_marshal_text(a, v, &err);
    if (BURROW_FAILED(err) || !str_eq(q(text), BURROW_S("3,4")))
        testing_t_errorf_v(t, "MarshalText = %q, %v; want \"3,4\", nil", q(text), err);

    Slice bin = encoding_marshal_binary(a, v, &err);
    if (BURROW_FAILED(err) || bin.len != 2 || ((Byte *)bin.p)[0] != 3 ||
        ((Byte *)bin.p)[1] != 4)
        testing_t_errorf_v(t, "MarshalBinary = %v, %v; want [3 4], nil", bin, err);

    Slice more = encoding_append_text(
        a, v, slice_append(a, slice_nil(TYPE_BYTE), "p=", 2), &err);
    if (BURROW_FAILED(err) || !str_eq(q(more), BURROW_S("p=3,4")))
        testing_t_errorf_v(t, "AppendText = %q, %v; want \"p=3,4\", nil", q(more), err);

    Point back = {0, 0};
    Any bv = BURROW_ANY(TYPE_OF(Point), &back);
    err = encoding_unmarshal_text(a, bv, BURROW_B("7,8"));
    if (BURROW_FAILED(err) || back.X != 7 || back.Y != 8)
        testing_t_errorf_v(t, "UnmarshalText gave {%d %d}, %v; want {7 8}, nil", back.X,
                           back.Y, err);
    err = encoding_unmarshal_binary(a, bv, bin);
    if (BURROW_FAILED(err) || back.X != 3 || back.Y != 4)
        testing_t_errorf_v(t, "UnmarshalBinary gave {%d %d}, %v; want {3 4}, nil",
                           back.X, back.Y, err);
    err = encoding_unmarshal_text(a, bv, BURROW_B("nope"));
    if (!BURROW_FAILED(err))
        testing_t_error_v(t, ("UnmarshalText(\"nope\") did not fail"));
    arena_free(&ar);
}

static void TestPointerFindsMethods(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Point p = {1, 2};
    PointPtr pp = &p;
    Any v = BURROW_ANY(&burrow_type_PointPtr, &pp);
    CHECK(encoding_is_text_marshaler(v));
    Error err = BURROW_NO_ERROR;
    Slice text = encoding_marshal_text(a, v, &err);
    if (!str_eq(q(text), BURROW_S("1,2")))
        testing_t_errorf_v(t, "MarshalText through a pointer = %q, want \"1,2\"",
                           q(text));
    err = encoding_unmarshal_text(a, v, BURROW_B("5,6"));
    if (BURROW_FAILED(err) || p.X != 5 || p.Y != 6)
        testing_t_errorf_v(t, "UnmarshalText through a pointer gave {%d %d}", p.X, p.Y);
    arena_free(&ar);
}

static void TestWrongSignature(TestingT *t) {
    Odd o = {1};
    Any v = BURROW_ANY(TYPE_OF(Odd), &o);
    if (encoding_is_text_marshaler(v))
        testing_t_error_v(t, ("a String-shaped MarshalText counted as one"));
    Error err = BURROW_NO_ERROR;
    Slice text = encoding_marshal_text(heap_allocator(), v, &err);
    if (!slice_is_nil(text))
        testing_t_errorf_v(t, "MarshalText on Odd = %q, want nil", q(text));
    Str want = BURROW_S(
        "interface conversion: Odd is not encoding.TextMarshaler: missing method "
        "MarshalText");
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), want))
        testing_t_errorf_v(t, "err = %v, want %q", err, want);
}

static void TestNotImplemented(TestingT *t) {
    Int n = 3;
    Any v = BURROW_ANY(TYPE_INT, &n);
    CHECK(!encoding_is_binary_marshaler(v));
    CHECK(!encoding_is_binary_unmarshaler(v));
    CHECK(!encoding_is_binary_appender(v));
    CHECK(!encoding_is_text_marshaler(v));
    CHECK(!encoding_is_text_unmarshaler(v));
    CHECK(!encoding_is_text_appender(v));

    Error err = encoding_unmarshal_binary(heap_allocator(), v, BURROW_B("x"));
    Str want = BURROW_S("interface conversion: int is not encoding.BinaryUnmarshaler: "
                        "missing method UnmarshalBinary");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "err = %v, want %q", err, want);

    Slice b = BURROW_B("keep");
    Slice got = encoding_append_binary(heap_allocator(), v, b, &err);
    if (got.p != b.p || got.len != b.len)
        testing_t_error_v(t, ("AppendBinary on int changed its input"));

    Any nil = {NULL, NULL};
    CHECK(!encoding_is_text_marshaler(nil));
    err = encoding_unmarshal_text(heap_allocator(), nil, BURROW_B("x"));
    want = BURROW_S(
        "interface conversion: interface is nil, not encoding.TextUnmarshaler");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "err = %v, want %q", err, want);
}

/* A marshaler that exists only as a vtable, held in an Any as the interface
 * value, which is how a caller that was handed an EncodingTextMarshaler passes
 * it on. */
static Slice upper_marshal_text(void *self, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = *(const Str *)self;
    return bytes_to_upper(a,
                          slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE));
}

static const EncodingTextMarshalerVT upper_vt = {NULL, upper_marshal_text};

static void TestInterfaceValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str word = BURROW_S("shout");
    EncodingTextMarshaler m = {&upper_vt, &word};
    Any v = BURROW_ANY(TYPE_OF(EncodingTextMarshaler), &m);
    CHECK(encoding_is_text_marshaler(v));
    CHECK(!encoding_is_text_unmarshaler(v));
    Error err = BURROW_NO_ERROR;
    Slice text = encoding_marshal_text(a, v, &err);
    if (!str_eq(q(text), BURROW_S("SHOUT")))
        testing_t_errorf_v(t, "MarshalText = %q, want \"SHOUT\"", q(text));

    EncodingTextMarshaler none = {NULL, NULL};
    Any nv = BURROW_ANY(TYPE_OF(EncodingTextMarshaler), &none);
    CHECK(!encoding_is_text_marshaler(nv));
    arena_free(&ar);
}

/* An interface value of some other type, error here, whose dynamic type has
 * the methods. */
#define PERR_FIELDS(F, T) F(T, Int, X, "")
BURROW_STRUCT_DECL(PointErr, PERR_FIELDS);

static Str perr_error(PointErr *p) {
    (void)p;
    return BURROW_S("point error");
}

static Slice perr_marshal_text(PointErr *p, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "perr %d", p->X);
    return slice_append(a, slice_nil(TYPE_BYTE), s.p, s.len);
}

#define PERR_SIG_Error(IN, OUT) OUT(Str)
#define PERR_METHODS(M, T)                                                             \
    M(T, Error, perr_error, PERR_SIG_Error)                                            \
    M(T, MarshalText, perr_marshal_text, ENCODING_SIG_MARSHAL_TEXT)

BURROW_STRUCT_DEFINE_METHODS(PointErr, PERR_FIELDS, PERR_METHODS);

static Str perr_vt_error(const void *self) {
    return perr_error((PointErr *)(uintptr_t)self);
}

static const ErrorVT perr_vt = {.self_type = &burrow_type_PointErr,
                                .message = perr_vt_error};

static void TestInsideAnInterface(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PointErr pe = {9};
    Error e = {&perr_vt, &pe};
    Any v = BURROW_ANY(TYPE_ERROR, &e);
    CHECK(encoding_is_text_marshaler(v));
    Error err = BURROW_NO_ERROR;
    Slice text = encoding_marshal_text(a, v, &err);
    if (!str_eq(q(text), BURROW_S("perr 9")))
        testing_t_errorf_v(t, "MarshalText = %q, want \"perr 9\"", q(text));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestMarshalThroughDescriptor)                                                    \
    X(TestPointerFindsMethods)                                                         \
    X(TestWrongSignature)                                                              \
    X(TestNotImplemented)                                                              \
    X(TestInterfaceValue)                                                              \
    X(TestInsideAnInterface)

TESTING_MAIN(TESTS)
