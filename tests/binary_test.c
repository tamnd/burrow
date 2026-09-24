/* Derived from Go's src/encoding/binary/binary_test.go and varint_test.go.
 * Go source: go1.27.1.
 *
 * Left out: TestReadInvalidDestination, because an Any here always names its
 * variable, so reading into an int32 value works rather than failing, and
 * TestSizeStructCache and TestSizeAllocs, which test a cache this does not
 * have and allocations Size cannot make since it takes no allocator.
 * TestAppendAllocs becomes TestAppendNoAlloc, which hands Append an allocator
 * with no room.
 *
 * The tests after the varint ones are new, and their expected values were
 * taken from Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/binary.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"

#include <string.h>

/* ------------------------------------------------------------------ types */

BURROW_ARRAY_TYPE(U8x4, uint8_t, 4);
BURROW_ARRAY_TYPE(Boolx4, bool, 4);

#define STRUCT_FIELDS(F, T)                                                            \
    F(T, int8_t, Int8, "")                                                             \
    F(T, int16_t, Int16, "")                                                           \
    F(T, int32_t, Int32, "")                                                           \
    F(T, int64_t, Int64, "")                                                           \
    F(T, uint8_t, Uint8, "")                                                           \
    F(T, uint16_t, Uint16, "")                                                         \
    F(T, uint32_t, Uint32, "")                                                         \
    F(T, uint64_t, Uint64, "")                                                         \
    F(T, float, Float32, "")                                                           \
    F(T, double, Float64, "")                                                          \
    F(T, Complex64, Cplx64, "")                                                        \
    F(T, Complex128, Cplx128, "")                                                      \
    F(T, U8x4, Array, "")                                                              \
    F(T, bool, Bool, "")                                                               \
    F(T, Boolx4, BoolArray, "")
BURROW_STRUCT(Struct, STRUCT_FIELDS);
BURROW_PTR_TYPE(StructPtr, Struct);
BURROW_SLICE_TYPE(StructSlice, Struct);

BURROW_ARRAY_TYPE(Intx4, Int, 4);

#define T_FIELDS(F, T)                                                                 \
    F(T, Int, Int, "")                                                                 \
    F(T, Uint, Uint, "")                                                               \
    F(T, Uintptr, Uintptr, "")                                                         \
    F(T, Intx4, Array, "")
BURROW_STRUCT(TT, T_FIELDS);

BURROW_SLICE_TYPE(I8Slice, int8_t);
BURROW_SLICE_TYPE(I16Slice, int16_t);
BURROW_SLICE_TYPE(I32Slice, int32_t);
BURROW_SLICE_TYPE(I64Slice, int64_t);
BURROW_SLICE_TYPE(U8Slice, uint8_t);
BURROW_SLICE_TYPE(U16Slice, uint16_t);
BURROW_SLICE_TYPE(U32Slice, uint32_t);
BURROW_SLICE_TYPE(U64Slice, uint64_t);
BURROW_SLICE_TYPE(F32Slice, float);
BURROW_SLICE_TYPE(BoolSlice, bool);
BURROW_SLICE_TYPE(IntSlice, Int);
BURROW_PTR_TYPE(I32SlicePtr, I32Slice);

static float f32frombits(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static double f64frombits(uint64_t b) {
    double f;
    memcpy(&f, &b, 8);
    return f;
}

static Struct s;

static void init_s(void) {
    memset(&s, 0, sizeof s);
    s.Int8 = 0x01;
    s.Int16 = 0x0203;
    s.Int32 = 0x04050607;
    s.Int64 = 0x08090a0b0c0d0e0f;
    s.Uint8 = 0x10;
    s.Uint16 = 0x1112;
    s.Uint32 = 0x13141516;
    s.Uint64 = 0x1718191a1b1c1d1e;
    s.Float32 = f32frombits(0x1f202122);
    s.Float64 = f64frombits(0x232425262728292a);
    s.Cplx64 = (Complex64){f32frombits(0x2b2c2d2e), f32frombits(0x2f303132)};
    s.Cplx128 =
        (Complex128){f64frombits(0x333435363738393a), f64frombits(0x3b3c3d3e3f404142)};
    s.Array = (U8x4){{0x43, 0x44, 0x45, 0x46}};
    s.Bool = true;
    s.BoolArray = (Boolx4){{true, false, true, false}};
}

static const Byte big[] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
    39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 1,  1,  0,  1,  0,
};

static const Byte little[] = {
    1,  3,  2,  7,  6,  5,  4,  15, 14, 13, 12, 11, 10, 9,  8,  16, 18, 17, 22,
    21, 20, 19, 30, 29, 28, 27, 26, 25, 24, 23, 34, 33, 32, 31, 42, 41, 40, 39,
    38, 37, 36, 35, 46, 45, 44, 43, 50, 49, 48, 47, 58, 57, 56, 55, 54, 53, 52,
    51, 66, 65, 64, 63, 62, 61, 60, 59, 67, 68, 69, 70, 1,  1,  0,  1,  0,
};

static const Byte src[] = {1, 2, 3, 4, 5, 6, 7, 8};
static const int32_t res[] = {0x01020304, 0x05060708};

#define BYTES(arr)                                                                     \
    slice_from((void *)(uintptr_t)(arr), (Int)sizeof(arr), (Int)sizeof(arr), TYPE_BYTE)

static bool bytes_eq(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

/* Field by field, so the padding between them does not count. */
static bool struct_eq(const Type *t, const void *a, const void *b) {
    for (uint16_t i = 0; i < t->nfield; i++) {
        const Field *f = &t->fields[i];
        if (memcmp((const Byte *)a + f->offset, (const Byte *)b + f->offset,
                   f->type->size) != 0)
            return false;
    }
    return true;
}

/* ------------------------------------------------- encoders and decoders */

typedef Slice (*EncFn)(Alloc *a, BinaryByteOrder o, Any data, Error *err);
typedef Error (*DecFn)(Alloc *a, BinaryByteOrder o, Any data, Slice buf);

static Slice enc_write(Alloc *a, BinaryByteOrder o, Any data, Error *err) {
    BytesBuffer buf = BYTES_BUFFER(a);
    *err = binary_write(a, bytes_buffer_as_io_writer(&buf), o, data);
    return bytes_buffer_bytes(&buf);
}

static Slice enc_encode(Alloc *a, BinaryByteOrder o, Any data, Error *err) {
    Int size = binary_size(data);
    Slice buf = {NULL, 0, 0, NULL};
    if (size > 0)
        buf = slice_make(a, TYPE_BYTE, size, size);
    Int n = binary_encode(buf, o, data, err);
    if (BURROW_OK(*err) && n != size)
        *err = fmt_errorf_v("returned size %d instead of %d", n, size);
    return buf;
}

static Slice enc_append(Alloc *a, BinaryByteOrder o, Any data, Error *err) {
    return binary_append(a, (Slice){NULL, 0, 0, NULL}, o, data, err);
}

static Error dec_read(Alloc *a, BinaryByteOrder o, Any data, Slice buf) {
    BytesReader r;
    bytes_reader_reset(&r, buf);
    return binary_read(a, bytes_reader_as_io_reader(&r), o, data);
}

static Error dec_decode(Alloc *a, BinaryByteOrder o, Any data, Slice buf) {
    (void)a;
    Error err;
    Int n = binary_decode(buf, o, data, &err);
    if (BURROW_OK(err) && n != binary_size(data))
        return fmt_errorf_v("returned size %d instead of %d", n, binary_size(data));
    return err;
}

static const struct {
    const char *name;
    EncFn fn;
} encoders[] = {{"Write", enc_write}, {"Encode", enc_encode}, {"Append", enc_append}};

static const struct {
    const char *name;
    DecFn fn;
} decoders[] = {{"Read", dec_read}, {"Decode", dec_decode}};

#define NENC ((int)(sizeof encoders / sizeof encoders[0]))
#define NDEC ((int)(sizeof decoders / sizeof decoders[0]))

static void test_read(TestingT *t, BinaryByteOrder order, Slice b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < NDEC; i++) {
        Struct s2;
        memset(&s2, 0, sizeof s2);
        Error err = decoders[i].fn(a, order, BURROW_ANY(TYPE_OF(Struct), &s2), b);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s %s: %s", decoders[i].name,
                               order.vt->string(order.data), error_text(err));
        else if (!struct_eq(TYPE_OF(Struct), &s2, &s))
            testing_t_errorf_v(t, "%s %s: struct differs", decoders[i].name,
                               order.vt->string(order.data));
    }
    arena_free(&ar);
}

static void test_write(TestingT *t, BinaryByteOrder order, Slice b, Any data) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < NENC; i++) {
        Error err;
        Slice buf = encoders[i].fn(a, order, data, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s %s: %s", encoders[i].name,
                               order.vt->string(order.data), error_text(err));
        else if (!bytes_eq(buf, b))
            testing_t_errorf_v(t, "%s %s:\n\thave %v\n\twant %v", encoders[i].name,
                               order.vt->string(order.data), buf, b);
    }
    arena_free(&ar);
}

static void TestLittleEndianRead(TestingT *t) {
    init_s();
    test_read(t, binary_little_endian, BYTES(little));
}

static void TestLittleEndianWrite(TestingT *t) {
    init_s();
    test_write(t, binary_little_endian, BYTES(little), BURROW_ANY(TYPE_OF(Struct), &s));
}

static void TestLittleEndianPtrWrite(TestingT *t) {
    init_s();
    StructPtr p = &s;
    test_write(t, binary_little_endian, BYTES(little),
               BURROW_ANY(TYPE_OF(StructPtr), &p));
}

static void TestBigEndianRead(TestingT *t) {
    init_s();
    test_read(t, binary_big_endian, BYTES(big));
}

static void TestBigEndianWrite(TestingT *t) {
    init_s();
    test_write(t, binary_big_endian, BYTES(big), BURROW_ANY(TYPE_OF(Struct), &s));
}

static void TestBigEndianPtrWrite(TestingT *t) {
    init_s();
    StructPtr p = &s;
    test_write(t, binary_big_endian, BYTES(big), BURROW_ANY(TYPE_OF(StructPtr), &p));
}

static void TestReadSlice(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < NDEC; i++) {
        int32_t v[2] = {0, 0};
        I32Slice slice = slice_from(v, 2, 2, TYPE_INT32);
        Error err = decoders[i].fn(a, binary_big_endian,
                                   BURROW_ANY(TYPE_OF(I32Slice), &slice), BYTES(src));
        if (BURROW_FAILED(err) || memcmp(v, res, sizeof v) != 0)
            testing_t_errorf_v(t, "ReadSlice %s: %v %d %d", decoders[i].name, err, v[0],
                               v[1]);
    }
    arena_free(&ar);
}

static void TestWriteSlice(TestingT *t) {
    I32Slice slice = slice_from((void *)(uintptr_t)res, 2, 2, TYPE_INT32);
    test_write(t, binary_big_endian, BYTES(src), BURROW_ANY(TYPE_OF(I32Slice), &slice));
}

static void TestReadBool(TestingT *t) {
    static const Byte in[] = {0, 1, 2};
    static const bool want[] = {false, true, true};
    for (int i = 0; i < NDEC; i++) {
        for (int j = 0; j < 3; j++) {
            bool got = false;
            Error err =
                decoders[i].fn(NULL, binary_big_endian, BURROW_ANY(TYPE_BOOL, &got),
                               slice_from((void *)(uintptr_t)&in[j], 1, 1, TYPE_BYTE));
            if (BURROW_FAILED(err) || got != want[j])
                testing_t_errorf_v(t, "%s %d: got %v, %v", decoders[i].name, (Int)in[j],
                                   got, err);
        }
    }
}

static void TestReadBoolSlice(TestingT *t) {
    static const Byte in[] = {0, 1, 2, 255};
    for (int i = 0; i < NDEC; i++) {
        bool v[4] = {true, false, false, false};
        BoolSlice slice = slice_from(v, 4, 4, TYPE_BOOL);
        Error err = decoders[i].fn(NULL, binary_big_endian,
                                   BURROW_ANY(TYPE_OF(BoolSlice), &slice), BYTES(in));
        if (BURROW_FAILED(err) || v[0] || !v[1] || !v[2] || !v[3])
            testing_t_errorf_v(t, "%s: got %v %v %v %v, %v", decoders[i].name, v[0],
                               v[1], v[2], v[3], err);
    }
}

static void TestSliceRoundTrip(TestingT *t) {
    static const struct {
        const Type *slice;
        const Type *elem;
    } kinds[] = {
        {TYPE_OF(I8Slice), TYPE_INT8},    {TYPE_OF(I16Slice), TYPE_INT16},
        {TYPE_OF(I32Slice), TYPE_INT32},  {TYPE_OF(I64Slice), TYPE_INT64},
        {TYPE_OF(U8Slice), TYPE_UINT8},   {TYPE_OF(U16Slice), TYPE_UINT16},
        {TYPE_OF(U32Slice), TYPE_UINT32}, {TYPE_OF(U64Slice), TYPE_UINT64},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int e = 0; e < NENC; e++) {
        for (int d = 0; d < NDEC; d++) {
            for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
                const Type *et = kinds[k].elem;
                Byte in[100 * 8], out[100 * 8];
                memset(out, 0, sizeof out);
                for (Int i = 0; i < 100; i++) {
                    uint64_t v = (uint64_t)(i * 0x07654321);
#if BURROW_LITTLE_ENDIAN
                    memcpy(in + (size_t)i * et->size, &v, et->size);
#else
                    memcpy(in + (size_t)i * et->size, (Byte *)&v + 8 - et->size,
                           et->size);
#endif
                }
                Slice sin = slice_from(in, 100, 100, et);
                Slice sout = slice_from(out, 100, 100, et);
                Error err;
                Slice buf = encoders[e].fn(a, binary_big_endian,
                                           BURROW_ANY(kinds[k].slice, &sin), &err);
                if (BURROW_OK(err))
                    err = decoders[d].fn(a, binary_big_endian,
                                         BURROW_ANY(kinds[k].slice, &sout), buf);
                if (BURROW_FAILED(err))
                    testing_t_fatalf_v(t, "%s,%s %s: %s", encoders[e].name,
                                       decoders[d].name, type_name(et),
                                       error_text(err));
                if (memcmp(in, out, 100 * et->size) != 0)
                    testing_t_fatalf_v(t, "%s,%s %s: differs", encoders[e].name,
                                       decoders[d].name, type_name(et));
            }
        }
    }
    arena_free(&ar);
}

static void TestWriteT(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TT ts;
    memset(&ts, 0, sizeof ts);
    for (int e = 0; e < NENC; e++) {
        Error err;
        encoders[e].fn(a, binary_big_endian, BURROW_ANY(TYPE_OF(TT), &ts), &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "WriteT: have err == nil, want non-nil");
        const Type *tt = TYPE_OF(TT);
        for (uint16_t i = 0; i < tt->nfield; i++) {
            const Field *f = &tt->fields[i];
            Any v = BURROW_ANY(f->type, (Byte *)&ts + f->offset);
            Str typ = fmt_sprintf_v(a, "%T", v);
            if (str_eq(typ, BURROW_S("[4]int")))
                typ = BURROW_S("int"); /* the problem is int, not the [4] */
            encoders[e].fn(a, binary_big_endian, v, &err);
            if (BURROW_OK(err))
                testing_t_errorf_v(t, "WriteT.%s: have err == nil, want non-nil", typ);
            else if (!strings_contains(error_text(err), typ))
                testing_t_errorf_v(t, "WriteT: have err == %q, want it to mention %s",
                                   error_text(err), typ);
        }
    }
    arena_free(&ar);
}

/* Go's
 *
 *     type BlankFields struct {
 *         A uint32
 *         _ int32
 *         B float64
 *         _ [4]int16
 *         C byte
 *         _ [7]byte
 *         _ struct {
 *             f [8]float32
 *         }
 *     }
 *
 * with the repeated _ spelled _1, _2 and _3, which field_is_blank treats the
 * same way. */
BURROW_ARRAY_TYPE(I16x4, int16_t, 4);
BURROW_ARRAY_TYPE(U8x7, uint8_t, 7);
BURROW_ARRAY_TYPE(F32x8, float, 8);

#define INNER_FIELDS(F, T) F(T, F32x8, f, "")
BURROW_STRUCT(Inner, INNER_FIELDS);

#define INNER_PROBE_FIELDS(F, T) F(T, F32x8, G, "")
BURROW_STRUCT(InnerProbe, INNER_PROBE_FIELDS);

#define BLANK_FIELDS(F, T)                                                             \
    F(T, uint32_t, A, "")                                                              \
    F(T, int32_t, _, "")                                                               \
    F(T, double, B, "")                                                                \
    F(T, I16x4, _1, "")                                                                \
    F(T, uint8_t, C, "")                                                               \
    F(T, U8x7, _2, "")                                                                 \
    F(T, Inner, _3, "")
BURROW_STRUCT(BlankFields, BLANK_FIELDS);
BURROW_PTR_TYPE(BlankFieldsPtr, BlankFields);

#define PROBE_FIELDS(F, T)                                                             \
    F(T, uint32_t, A, "")                                                              \
    F(T, int32_t, P0, "")                                                              \
    F(T, double, B, "")                                                                \
    F(T, I16x4, P1, "")                                                                \
    F(T, uint8_t, C, "")                                                               \
    F(T, U8x7, P2, "")                                                                 \
    F(T, InnerProbe, P3, "")
BURROW_STRUCT(BlankFieldsProbe, PROBE_FIELDS);
BURROW_PTR_TYPE(BlankFieldsProbePtr, BlankFieldsProbe);

static void TestBlankFields(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int e = 0; e < NENC; e++) {
        BlankFields b1;
        /* Something other than zero in the blank fields, so that writing zeros
         * for them is tested rather than assumed. */
        memset(&b1, 0x5a, sizeof b1);
        b1.A = 1234567890;
        b1.B = 2.718281828;
        b1.C = 42;
        BlankFieldsPtr pb1 = &b1;
        Error err;
        Slice buf = encoders[e].fn(a, binary_little_endian,
                                   BURROW_ANY(TYPE_OF(BlankFieldsPtr), &pb1), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: %s", encoders[e].name, error_text(err));

        /* zero values must have been written for blank fields */
        BlankFieldsProbe p;
        memset(&p, 0x77, sizeof p);
        BlankFieldsProbePtr pp = &p;
        err = dec_read(a, binary_little_endian,
                       BURROW_ANY(TYPE_OF(BlankFieldsProbePtr), &pp), buf);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: %s", encoders[e].name, error_text(err));
        if (p.P0 != 0 || p.P1.v[0] != 0 || p.P2.v[0] != 0 || p.P3.G.v[0] != 0)
            testing_t_errorf_v(t, "%s: non-zero values for originally blank fields",
                               encoders[e].name);

        /* write p and see if we can probe only some fields */
        buf = encoders[e].fn(a, binary_little_endian,
                             BURROW_ANY(TYPE_OF(BlankFieldsProbePtr), &pp), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: %s", encoders[e].name, error_text(err));

        /* read should ignore blank fields in b2 */
        BlankFields b2;
        memset(&b2, 0, sizeof b2);
        BlankFieldsPtr pb2 = &b2;
        err = dec_read(a, binary_little_endian,
                       BURROW_ANY(TYPE_OF(BlankFieldsPtr), &pb2), buf);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: %s", encoders[e].name, error_text(err));
        if (b1.A != b2.A || b1.B != b2.B || b1.C != b2.C)
            testing_t_errorf_v(t, "%s: fields differ after the round trip",
                               encoders[e].name);
    }
    arena_free(&ar);
}

BURROW_PTR_TYPE(IntPtr, Int);
BURROW_ARRAY_TYPE(Uintx1, Uint, 1);
BURROW_PTR_TYPE(Uintx1Ptr, Uintx1);
BURROW_PTR_TYPE(IntSlicePtr, IntSlice);
BURROW_PTR_TYPE(I8Ptr, int8_t);
BURROW_PTR_TYPE(U8Ptr, uint8_t);
BURROW_PTR_TYPE(I16Ptr, int16_t);
BURROW_PTR_TYPE(U16Ptr, uint16_t);
BURROW_PTR_TYPE(I32Ptr, int32_t);
BURROW_PTR_TYPE(U32Ptr, uint32_t);
BURROW_PTR_TYPE(I64Ptr, int64_t);
BURROW_PTR_TYPE(U64Ptr, uint64_t);
BURROW_PTR_TYPE(F32Ptr, float);
BURROW_PTR_TYPE(F64Ptr, double);
BURROW_PTR_TYPE(C64Ptr, Complex64);
BURROW_PTR_TYPE(C128Ptr, Complex128);
BURROW_PTR_TYPE(BoolPtr, bool);

static void TestSizeInvalid(TestingT *t) {
    Int zero = 0;
    IntPtr newint = &zero, nilint = NULL;
    Uintx1 arr = {{0}};
    Uintx1Ptr newarr = &arr, nilarr = NULL;
    IntSlice empty = slice_from(&zero, 0, 0, TYPE_INT), nilslice = {NULL, 0, 0, NULL};
    IntSlicePtr newslice = &empty, nilsliceptr = NULL;
    void *nilp = NULL;
    const Any cases[] = {
        BURROW_ANY(TYPE_INT, &zero),
        BURROW_ANY(TYPE_OF(IntPtr), &newint),
        BURROW_ANY(TYPE_OF(IntPtr), &nilint),
        BURROW_ANY(TYPE_OF(Uintx1), &arr),
        BURROW_ANY(TYPE_OF(Uintx1Ptr), &newarr),
        BURROW_ANY(TYPE_OF(Uintx1Ptr), &nilarr),
        BURROW_ANY(TYPE_OF(IntSlice), &empty),
        BURROW_ANY(TYPE_OF(IntSlice), &nilslice),
        BURROW_ANY(TYPE_OF(IntSlicePtr), &newslice),
        BURROW_ANY(TYPE_OF(IntSlicePtr), &nilsliceptr),
        BURROW_ANY(TYPE_OF(I8Ptr), &nilp),
        BURROW_ANY(TYPE_OF(U8Ptr), &nilp),
        BURROW_ANY(TYPE_OF(I16Ptr), &nilp),
        BURROW_ANY(TYPE_OF(U16Ptr), &nilp),
        BURROW_ANY(TYPE_OF(I32Ptr), &nilp),
        BURROW_ANY(TYPE_OF(U32Ptr), &nilp),
        BURROW_ANY(TYPE_OF(I64Ptr), &nilp),
        BURROW_ANY(TYPE_OF(U64Ptr), &nilp),
        BURROW_ANY(TYPE_OF(F32Ptr), &nilp),
        BURROW_ANY(TYPE_OF(F64Ptr), &nilp),
        BURROW_ANY(TYPE_OF(C64Ptr), &nilp),
        BURROW_ANY(TYPE_OF(C128Ptr), &nilp),
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Int got = binary_size(cases[i]);
        if (got != -1)
            testing_t_errorf_v(t, "Size(%T) = %d, want -1", cases[i], got);
    }
}

/* The panic's text, copied out while the panic value is still there to read
 * it, since the value is gone once the block ends. */
static char panic_buf[256];

static Str recovered(Func f) {
    volatile Int n = 0;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        Str m = panic_text(r);
        n = m.len < (Int)sizeof panic_buf ? m.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, m.p, (size_t)n);
    }
    BURROW_TRY_END;
    return str_from_bytes((const Byte *)panic_buf, n);
}

/* An attempt to read into a struct with an unexported field will panic. This
 * is probably not the best choice, but at this point anything else would be
 * an API change. */
#define UNEXPORTED_FIELDS(F, T) F(T, int32_t, a, "")
BURROW_STRUCT(Unexported, UNEXPORTED_FIELDS);
BURROW_PTR_TYPE(UnexportedPtr, Unexported);

static Slice unexported_buf;
static int unexported_dec;

static void run_unexported(void *arg) {
    (void)arg;
    Unexported u2 = {0};
    UnexportedPtr p = &u2;
    decoders[unexported_dec].fn(NULL, binary_little_endian,
                                BURROW_ANY(TYPE_OF(UnexportedPtr), &p), unexported_buf);
}

static void TestUnexportedRead(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    Unexported u1 = {1};
    UnexportedPtr p = &u1;
    Error err = binary_write(a, bytes_buffer_as_io_writer(&buf), binary_little_endian,
                             BURROW_ANY(TYPE_OF(UnexportedPtr), &p));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    unexported_buf = bytes_buffer_bytes(&buf);
    for (unexported_dec = 0; unexported_dec < NDEC; unexported_dec++) {
        Str v = recovered(BURROW_FN(Func, run_unexported, NULL));
        if (!str_eq(v,
                    BURROW_S("reflect: reflect.Value.SetInt using value obtained using "
                             "unexported field")))
            testing_t_fatalf_v(t, "%s: panic %q", decoders[unexported_dec].name, v);
    }
    arena_free(&ar);
}

BURROW_PTR_TYPE(UnexportedPtrPtr, UnexportedPtr);

static void TestReadErrorMsg(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int zero = 0;
    Unexported st = {0};
    UnexportedPtr sp = &st;
    UnexportedPtrPtr spp = &sp;
    const Any cases[] = {
        BURROW_ANY(TYPE_INT, &zero),
        BURROW_ANY(TYPE_OF(UnexportedPtrPtr), &spp),
    };
    for (int d = 0; d < NDEC; d++) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            Error err = decoders[d].fn(a, binary_little_endian, cases[i],
                                       (Slice){NULL, 0, 0, NULL});
            Str want = fmt_sprintf_v(a, "binary.%s: invalid type %T",
                                     str_from_cstr(decoders[d].name), cases[i]);
            if (BURROW_OK(err))
                testing_t_errorf_v(t, "%T: got no error; want %q", cases[i], want);
            else if (!str_eq(error_text(err), want))
                testing_t_errorf_v(t, "%T: got %q; want %q", cases[i], error_text(err),
                                   want);
        }
    }
    arena_free(&ar);
}

#define TRUNC_FIELDS(F, T)                                                             \
    F(T, uint8_t, A, "")                                                               \
    F(T, uint8_t, B, "")                                                               \
    F(T, uint8_t, C, "")                                                               \
    F(T, uint8_t, D, "")                                                               \
    F(T, int32_t, E, "")                                                               \
    F(T, double, G, "")
BURROW_STRUCT(Trunc, TRUNC_FIELDS);
BURROW_PTR_TYPE(TruncPtr, Trunc);

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static void TestReadTruncated(TestingT *t) {
    static const char data[] = "0123456789abcdef";
    const Int n = (Int)sizeof data - 1;
    int32_t v1[4];
    I32Slice b1 = slice_from(v1, 4, 4, TYPE_INT32);
    I32SlicePtr pb1 = &b1;
    Trunc b2;
    TruncPtr pb2 = &b2;
    for (Int i = 0; i <= n; i++) {
        Error want = i == 0 ? io_eof : i == n ? BURROW_NO_ERROR : io_err_unexpected_eof;
        StringsReader r;
        strings_reader_reset(&r, str_from_bytes((const Byte *)data, i));
        Error err =
            binary_read(NULL, strings_reader_as_io_reader(&r), binary_little_endian,
                        BURROW_ANY(TYPE_OF(I32SlicePtr), &pb1));
        if (!same_error(err, want))
            testing_t_errorf_v(t, "Read(%d) with slice: got %v, want %v", i, err, want);
        strings_reader_reset(&r, str_from_bytes((const Byte *)data, i));
        err = binary_read(NULL, strings_reader_as_io_reader(&r), binary_little_endian,
                          BURROW_ANY(TYPE_OF(TruncPtr), &pb2));
        if (!same_error(err, want))
            testing_t_errorf_v(t, "Read(%d) with struct: got %v, want %v", i, err,
                               want);
    }
}

static void TestByteOrder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte room[8];
    static const uint64_t values[] = {
        0x0000000000000000, 0x0123456789abcdef, 0xfedcba9876543210,
        0xffffffffffffffff, 0xaaaaaaaaaaaaaaaa, 0x400921fb54442d18, /* Pi */
        0x4005bf0a8b145769,                                         /* E */
    };
    const BinaryByteOrder orders[] = {binary_little_endian, binary_big_endian};
    const BinaryAppendByteOrder aorders[] = {binary_little_endian_append,
                                             binary_big_endian_append};
    for (int o = 0; o < 2; o++) {
        BinaryByteOrder order = orders[o];
        BinaryAppendByteOrder ao = aorders[o];
        const Int offset = 3;
        Slice buf = slice_from(room, 8, 8, TYPE_BYTE);
        for (size_t k = 0; k < sizeof values / sizeof values[0]; k++) {
            uint64_t value = values[k];

            uint16_t want16 = (uint16_t)value;
            order.vt->put_uint16(order.data, slice_sub(buf, 0, 2), want16);
            if (order.vt->uint16(order.data, slice_sub(buf, 0, 2)) != want16)
                testing_t_errorf_v(t, "PutUint16: Uint16 wrong for %d", (Int)want16);
            buf = ao.vt->append_uint16(ao.data, a, slice_sub(buf, 0, offset), want16);
            if (order.vt->uint16(order.data, slice_sub(buf, offset, buf.len)) != want16)
                testing_t_errorf_v(t, "AppendUint16: Uint16 wrong for %d", (Int)want16);
            if (buf.len != offset + 2)
                testing_t_errorf_v(t, "AppendUint16: len(buf) = %d, want %d", buf.len,
                                   offset + 2);

            uint32_t want32 = (uint32_t)value;
            order.vt->put_uint32(order.data, slice_sub(buf, 0, 4), want32);
            if (order.vt->uint32(order.data, slice_sub(buf, 0, 4)) != want32)
                testing_t_errorf_v(t, "PutUint32: Uint32 wrong for %d", (Int)want32);
            buf = ao.vt->append_uint32(ao.data, a, slice_sub(buf, 0, offset), want32);
            if (order.vt->uint32(order.data, slice_sub(buf, offset, buf.len)) != want32)
                testing_t_errorf_v(t, "AppendUint32: Uint32 wrong for %d", (Int)want32);
            if (buf.len != offset + 4)
                testing_t_errorf_v(t, "AppendUint32: len(buf) = %d, want %d", buf.len,
                                   offset + 4);

            uint64_t want64 = value;
            order.vt->put_uint64(order.data, slice_sub(buf, 0, 8), want64);
            if (order.vt->uint64(order.data, slice_sub(buf, 0, 8)) != want64)
                testing_t_errorf_v(t, "PutUint64: Uint64 wrong for %v", want64);
            buf = ao.vt->append_uint64(ao.data, a, slice_sub(buf, 0, offset), want64);
            if (order.vt->uint64(order.data, slice_sub(buf, offset, buf.len)) != want64)
                testing_t_errorf_v(t, "AppendUint64: Uint64 wrong for %v", want64);
            if (buf.len != offset + 8)
                testing_t_errorf_v(t, "AppendUint64: len(buf) = %d, want %d", buf.len,
                                   offset + 8);
        }
    }
    arena_free(&ar);
}

static void run_uint64_small(void *arg) {
    (void)arg;
    Byte b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    binary_little_endian_uint64(slice_from(b, 4, 8, TYPE_BYTE));
}

static void run_put_uint64_small(void *arg) {
    (void)arg;
    Byte b[8] = {0};
    binary_little_endian_put_uint64(slice_from(b, 4, 8, TYPE_BYTE), 0x0102030405060708);
}

static void TestEarlyBoundsChecks(TestingT *t) {
    Str want = BURROW_S("runtime error: index out of range [7] with length 4");
    if (!str_eq(recovered(BURROW_FN(Func, run_uint64_small, NULL)), want))
        testing_t_errorf_v(t, "binary.LittleEndian.Uint64 expected to panic for small "
                              "slices, but didn't");
    if (!str_eq(recovered(BURROW_FN(Func, run_put_uint64_small, NULL)), want))
        testing_t_errorf_v(t,
                           "binary.LittleEndian.PutUint64 expected to panic for small "
                           "slices, but didn't");
}

#define PERSON_FIELDS(F, T)                                                            \
    F(T, Int, Age, "")                                                                 \
    F(T, double, Weight, "")                                                           \
    F(T, double, Height, "")
BURROW_STRUCT(Person, PERSON_FIELDS);
BURROW_PTR_TYPE(PersonPtr, Person);

static void TestNoFixedSize(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Person person = {27, 67.3, 177.8};
    PersonPtr p = &person;
    Any data = BURROW_ANY(TYPE_OF(PersonPtr), &p);
    for (int e = 0; e < NENC; e++) {
        Error err;
        encoders[e].fn(a, binary_little_endian, data, &err);
        if (BURROW_OK(err))
            testing_t_fatalf_v(
                t,
                "binary.%s: unexpected success as size of type %T is not "
                "fixed",
                str_from_cstr(encoders[e].name), data);
        Str errs =
            fmt_sprintf_v(a, "binary.%s: some values are not fixed-sized in type %T",
                          str_from_cstr(encoders[e].name), data);
        if (!str_eq(error_text(err), errs))
            testing_t_fatalf_v(t, "got %q, want %q", error_text(err), errs);
    }
    arena_free(&ar);
}

/* Go checks that Append into a buffer with room allocates nothing. Here the
 * allocator it gets has no room at all, so any allocation would fail. */
static void TestAppendNoAlloc(TestingT *t) {
    init_s();
    static unsigned char room[1];
    Fixed fx;
    fixed_init(&fx, room, sizeof room);
    Alloc *a = fixed_allocator(&fx);
    StructPtr p = &s;
    Any data = BURROW_ANY(TYPE_OF(StructPtr), &p);
    Byte backing[128];
    Slice buf = slice_from(backing, 0, binary_size(data), TYPE_BYTE);
    Error err;
    Slice out = binary_append(a, buf, binary_little_endian, data, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Append failed: %s", error_text(err));
    if (out.p != backing || !bytes_eq(out, BYTES(little)))
        testing_t_fatalf_v(t, "Append did not fill the buffer it was given");
}

static void TestNativeEndian(TestingT *t) {
    const uint32_t val = 0x12345678;
    uint32_t i = val;
    Slice sl = slice_from(&i, 4, 4, TYPE_BYTE);
    uint32_t v = binary_native_endian.vt->uint32(binary_native_endian.data, sl);
    if (v != val)
        testing_t_errorf_v(t, "NativeEndian.Uint32 returned %#x, expected %#x", v, val);
    if (binary_native_endian_uint32(sl) != val)
        testing_t_errorf_v(t, "binary_native_endian_uint32 returned the wrong value");
}

/* ---------------------------------------------------------------- varints */

static void test_constant(TestingT *t, unsigned w, Int max) {
    Byte b[BINARY_MAX_VARINT_LEN64];
    uint64_t x = w == 64 ? ~(uint64_t)0 : ((uint64_t)1 << w) - 1;
    Int n = binary_put_uvarint(slice_from(b, sizeof b, sizeof b, TYPE_BYTE), x);
    if (n != max)
        testing_t_errorf_v(t, "MaxVarintLen%d = %d; want %d", (Int)w, max, n);
}

static void TestConstants(TestingT *t) {
    test_constant(t, 16, BINARY_MAX_VARINT_LEN16);
    test_constant(t, 32, BINARY_MAX_VARINT_LEN32);
    test_constant(t, 64, BINARY_MAX_VARINT_LEN64);
}

static void test_varint(TestingT *t, Alloc *a, int64_t x) {
    Byte b[BINARY_MAX_VARINT_LEN64] = {0};
    Slice buf = slice_from(b, sizeof b, sizeof b, TYPE_BYTE);
    Int n = binary_put_varint(buf, x);
    Int m;
    int64_t y = binary_varint(slice_sub(buf, 0, n), &m);
    if (x != y)
        testing_t_errorf_v(t, "Varint(%d): got %d", x, y);
    if (n != m)
        testing_t_errorf_v(t, "Varint(%d): got n = %d; want %d", x, m, n);

    Slice buf2 = slice_append(a, slice_nil(TYPE_BYTE), "prefix", 6);
    buf2 = binary_append_varint(a, buf2, x);
    if (buf2.len != 6 + n || memcmp((Byte *)buf2.p + 6, b, (size_t)n) != 0)
        testing_t_errorf_v(t, "AppendVarint(%d): got %v", x, buf2);

    BytesReader r;
    bytes_reader_reset(&r, buf);
    Error err;
    y = binary_read_varint(bytes_reader_as_io_byte_reader(&r), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadVarint(%d): %s", x, error_text(err));
    if (x != y)
        testing_t_errorf_v(t, "ReadVarint(%d): got %d", x, y);
}

static void test_uvarint(TestingT *t, Alloc *a, uint64_t x) {
    Byte b[BINARY_MAX_VARINT_LEN64] = {0};
    Slice buf = slice_from(b, sizeof b, sizeof b, TYPE_BYTE);
    Int n = binary_put_uvarint(buf, x);
    Int m;
    uint64_t y = binary_uvarint(slice_sub(buf, 0, n), &m);
    if (x != y)
        testing_t_errorf_v(t, "Uvarint(%d): got %d", x, y);
    if (n != m)
        testing_t_errorf_v(t, "Uvarint(%d): got n = %d; want %d", x, m, n);

    Slice buf2 = slice_append(a, slice_nil(TYPE_BYTE), "prefix", 6);
    buf2 = binary_append_uvarint(a, buf2, x);
    if (buf2.len != 6 + n || memcmp((Byte *)buf2.p + 6, b, (size_t)n) != 0)
        testing_t_errorf_v(t, "AppendUvarint(%d): got %v", x, buf2);

    BytesReader r;
    bytes_reader_reset(&r, buf);
    Error err;
    y = binary_read_uvarint(bytes_reader_as_io_byte_reader(&r), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadUvarint(%d): %s", x, error_text(err));
    if (x != y)
        testing_t_errorf_v(t, "ReadUvarint(%d): got %d", x, y);
}

static const int64_t tests[] = {
    INT64_MIN, INT64_MIN + 1, -1,  0,   1,   2,   10,  20,        63, 64,
    65,        127,           128, 129, 255, 256, 257, INT64_MAX,
};

static void TestVarint(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        test_varint(t, a, tests[i]);
        test_varint(t, a, (int64_t)(0 - (uint64_t)tests[i]));
    }
    for (uint64_t x = 0x7; x != 0; x <<= 1) {
        test_varint(t, a, (int64_t)x);
        test_varint(t, a, (int64_t)(0 - x));
    }
    arena_free(&ar);
}

static void TestUvarint(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        test_uvarint(t, a, (uint64_t)tests[i]);
    for (uint64_t x = 0x7; x != 0; x <<= 1)
        test_uvarint(t, a, x);
    arena_free(&ar);
}

static void TestBufferTooSmall(TestingT *t) {
    Byte b[] = {0x80, 0x80, 0x80, 0x80};
    for (Int i = 0; i <= 4; i++) {
        Slice buf = slice_from(b, i, i, TYPE_BYTE);
        Int n;
        uint64_t x = binary_uvarint(buf, &n);
        if (x != 0 || n != 0)
            testing_t_errorf_v(t, "Uvarint(%v): got x = %d, n = %d", buf, x, n);

        BytesReader r;
        bytes_reader_reset(&r, buf);
        Error err;
        x = binary_read_uvarint(bytes_reader_as_io_byte_reader(&r), &err);
        Error want = i > 0 ? io_err_unexpected_eof : io_eof;
        if (x != 0 || !same_error(err, want))
            testing_t_errorf_v(t, "ReadUvarint(%v): got x = %d, err = %v", buf, x, err);
    }
}

static void TestBufferTooBigWithOverflow(TestingT *t) {
    static Byte thousand[1000];
    memset(thousand, 0xff, sizeof thousand);
    thousand[999] = 0;
    static Byte valid[] = {0xd7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01};
    static Byte more[] = {0xd7, 0xff, 0xff, 0xff, 0xff, 0xff,
                          0xff, 0xff, 0xff, 0xff, 0x01};
    static Byte tenth[] = {0xd7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
    const struct {
        Slice in;
        const char *name;
        Int want_n;
        uint64_t want_value;
    } cases[] = {
        {BYTES(thousand), "invalid: 1000 bytes", -11, 0},
        {BYTES(valid), "valid: math.MaxUint64-40", 10, UINT64_MAX - 40},
        {BYTES(more), "invalid: with more than MaxVarintLen64 bytes", -11, 0},
        {BYTES(tenth), "invalid: 10th byte", -10, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Int n;
        uint64_t value = binary_uvarint(cases[i].in, &n);
        if (n != cases[i].want_n)
            testing_t_errorf_v(t, "%s: bytes returned=%d, want=%d",
                               str_from_cstr(cases[i].name), n, cases[i].want_n);
        if (value != cases[i].want_value)
            testing_t_errorf_v(t, "%s: value=%d, want=%d", str_from_cstr(cases[i].name),
                               value, cases[i].want_value);
    }
}

static void test_overflow(TestingT *t, Slice buf, uint64_t x0, Int n0) {
    Int n;
    uint64_t x = binary_uvarint(buf, &n);
    if (x != 0 || n != n0)
        testing_t_errorf_v(t, "Uvarint(% X): got x = %d, n = %d; want 0, %d", buf, x, n,
                           n0);

    BytesReader r;
    bytes_reader_reset(&r, buf);
    Int len = bytes_reader_len(&r);
    Error err;
    x = binary_read_uvarint(bytes_reader_as_io_byte_reader(&r), &err);
    if (x != x0 ||
        !str_eq(error_text(err), BURROW_S("binary: varint overflows a 64-bit "
                                          "integer")))
        testing_t_errorf_v(t, "ReadUvarint(%v): got x = %d, err = %v; want %d", buf, x,
                           err, x0);
    Int read = len - bytes_reader_len(&r);
    if (read > BINARY_MAX_VARINT_LEN64)
        testing_t_errorf_v(
            t, "ReadUvarint(%v): read more than MaxVarintLen64 bytes, got %d", buf,
            read);
}

static void TestOverflow(TestingT *t) {
    static Byte a[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x2};
    static Byte b[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                       0x80, 0x80, 0x80, 0x80, 0x1,  0,    0};
    static Byte c[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    test_overflow(t, BYTES(a), 0, -10);
    test_overflow(t, BYTES(b), 0, -11);
    test_overflow(t, BYTES(c), UINT64_MAX, -11); /* 11 bytes, should overflow */
}

static void TestNonCanonicalZero(TestingT *t) {
    static Byte b[] = {0x80, 0x80, 0x80, 0};
    Int n;
    uint64_t x = binary_uvarint(BYTES(b), &n);
    if (x != 0 || n != 4)
        testing_t_errorf_v(t, "Uvarint(%v): got x = %d, n = %d; want 0, 4", BYTES(b), x,
                           n);
}

/* -------------------------------------------------------------- new tests */

static void run_uint16_short(void *arg) {
    (void)arg;
    Byte b[1] = {1};
    binary_big_endian_uint16(slice_from(b, 1, 1, TYPE_BYTE));
}

static void run_put_uvarint_short(void *arg) {
    (void)arg;
    Byte b[1];
    binary_put_uvarint(slice_from(b, 1, 1, TYPE_BYTE), 300);
}

static void run_nil_write(void *arg) {
    (void)arg;
    I32Ptr p = NULL;
    BytesBuffer buf = BYTES_BUFFER(NULL);
    binary_write(NULL, bytes_buffer_as_io_writer(&buf), binary_big_endian,
                 BURROW_ANY(TYPE_OF(I32Ptr), &p));
}

static void run_nil_read(void *arg) {
    (void)arg;
    I32Ptr p = NULL;
    BytesReader r;
    bytes_reader_reset(&r, BYTES(src));
    binary_read(NULL, bytes_reader_as_io_reader(&r), binary_big_endian,
                BURROW_ANY(TYPE_OF(I32Ptr), &p));
}

static void run_nil_any(void *arg) {
    (void)arg;
    Byte b[8];
    binary_encode(BYTES(b), binary_big_endian, (Any){NULL, NULL}, NULL);
}

static void TestPanics(TestingT *t) {
    static const struct {
        void (*fn)(void *);
        const char *want;
    } cases[] = {
        {run_uint16_short, "runtime error: index out of range [1] with length 1"},
        {run_put_uvarint_short, "runtime error: index out of range [1] with length 1"},
        {run_nil_write,
         "runtime error: invalid memory address or nil pointer dereference"},
        {run_nil_read,
         "runtime error: invalid memory address or nil pointer dereference"},
        {run_nil_any,
         "runtime error: invalid memory address or nil pointer dereference"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Str v = recovered(BURROW_FN(Func, cases[i].fn, NULL));
        if (!str_eq(v, str_from_cstr(cases[i].want)))
            testing_t_errorf_v(t, "#%d: panic %q, want %q", (Int)i, v,
                               str_from_cstr(cases[i].want));
    }
}

/* A nil pointer to a struct is an error rather than a panic, since Go's reflect
 * path finds no value there to size. */
static void TestNilStructPointer(TestingT *t) {
    StructPtr p = NULL;
    Any data = BURROW_ANY(TYPE_OF(StructPtr), &p);
    Byte b[128];
    Error err;
    CHECK(binary_size(data) == -1);
    CHECK(binary_encode(BYTES(b), binary_big_endian, data, &err) == 0);
    CHECK(str_eq(
        error_text(err),
        BURROW_S("binary.Encode: some values are not fixed-sized in type *Struct")));
    CHECK(binary_decode(BYTES(b), binary_big_endian, data, &err) == 0);
    CHECK(str_eq(error_text(err), BURROW_S("binary.Decode: invalid type *Struct")));
    CHECK(binary_size((Any){NULL, NULL}) == -1);
}

#define PADDED_FIELDS(F, T)                                                            \
    F(T, int32_t, A, "")                                                               \
    F(T, int16_t, _, "")
BURROW_STRUCT(Padded, PADDED_FIELDS);

/* No padding is written, and a blank field is zeros. */
static void TestPaddedAppend(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Padded v = {5, 7};
    Any data = BURROW_ANY(TYPE_OF(Padded), &v);
    CHECK(binary_size(data) == 6);
    Byte nine = 9;
    Slice buf = slice_append(a, slice_nil(TYPE_BYTE), &nine, 1);
    Error err;
    buf = binary_append(a, buf, binary_big_endian, data, &err);
    static const Byte want[] = {9, 0, 0, 0, 5, 0, 0};
    CHECK(BURROW_OK(err) && bytes_eq(buf, BYTES(want)));
    arena_free(&ar);
}

static void TestOrderNames(TestingT *t) {
    CHECK(str_eq(binary_little_endian.vt->string(NULL), BURROW_S("LittleEndian")));
    CHECK(str_eq(binary_big_endian.vt->string(NULL), BURROW_S("BigEndian")));
    CHECK(str_eq(binary_native_endian.vt->string(NULL), BURROW_S("NativeEndian")));
    CHECK(str_eq(binary_big_endian_append.vt->string(NULL), BURROW_S("BigEndian")));
    CHECK(str_eq(binary_little_endian_go_string(), BURROW_S("binary.LittleEndian")));
    CHECK(str_eq(binary_big_endian_go_string(), BURROW_S("binary.BigEndian")));
    CHECK(str_eq(binary_native_endian_go_string(), BURROW_S("binary.NativeEndian")));
}

/* A byte order from outside the package, big endian done the slow way, which
 * has to give the same bytes as the real one. */
static int custom_calls;

static uint16_t cu16(void *self, Slice b) {
    (void)self;
    custom_calls++;
    const Byte *p = (const Byte *)b.p;
    return (uint16_t)(p[0] << 8 | p[1]);
}
static uint32_t cu32(void *self, Slice b) {
    (void)self;
    custom_calls++;
    const Byte *p = (const Byte *)b.p;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t cu64(void *self, Slice b) {
    (void)self;
    custom_calls++;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | ((const Byte *)b.p)[i];
    return v;
}
static void cput16(void *self, Slice b, uint16_t v) {
    (void)self;
    custom_calls++;
    ((Byte *)b.p)[0] = (Byte)(v >> 8);
    ((Byte *)b.p)[1] = (Byte)v;
}
static void cput32(void *self, Slice b, uint32_t v) {
    (void)self;
    custom_calls++;
    for (int i = 0; i < 4; i++)
        ((Byte *)b.p)[i] = (Byte)(v >> (24 - 8 * i));
}
static void cput64(void *self, Slice b, uint64_t v) {
    (void)self;
    custom_calls++;
    for (int i = 0; i < 8; i++)
        ((Byte *)b.p)[i] = (Byte)(v >> (56 - 8 * i));
}
static Str cstring(void *self) {
    (void)self;
    return BURROW_S("Custom");
}

static const BinaryByteOrderVT custom_vt = {NULL,   cu16,   cu32,   cu64,
                                            cput16, cput32, cput64, cstring};

static void TestCustomOrder(TestingT *t) {
    init_s();
    BinaryByteOrder custom = {&custom_vt, NULL};
    custom_calls = 0;
    test_write(t, custom, BYTES(big), BURROW_ANY(TYPE_OF(Struct), &s));
    CHECK(custom_calls > 0);
    custom_calls = 0;
    test_read(t, custom, BYTES(big));
    CHECK(custom_calls > 0);
}

static Slice write_seen;

static Int grab_write(void *self, Slice p, Error *err) {
    (void)self;
    write_seen = p;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT grab_vt = {NULL, grab_write};

/* A []byte goes to the writer as it is, as it does in Go, and a failing
 * writer's error comes back. */
static void TestWriteBytes(TestingT *t) {
    Byte b[3] = {1, 2, 3};
    U8Slice sl = slice_from(b, 3, 3, TYPE_UINT8);
    Error err = binary_write(NULL, (IoWriter){&grab_vt, NULL}, binary_big_endian,
                             BURROW_ANY(TYPE_OF(U8Slice), &sl));
    CHECK(BURROW_OK(err) && write_seen.p == b && write_seen.len == 3);
}

/* A value bigger than the stack buffer goes through the allocator, and one
 * with no room is reported rather than written past. */
static void TestBigValue(TestingT *t) {
    static unsigned char room[1];
    Fixed fx;
    fixed_init(&fx, room, sizeof room);
    Alloc *none = fixed_allocator(&fx);
    int32_t v[100] = {0};
    I32Slice sl = slice_from(v, 100, 100, TYPE_INT32);
    Any data = BURROW_ANY(TYPE_OF(I32Slice), &sl);
    BytesBuffer out = BYTES_BUFFER(NULL);
    CHECK(errors_is(
        binary_write(none, bytes_buffer_as_io_writer(&out), binary_big_endian, data),
        burrow_err_out_of_memory));
    Byte in[400] = {0};
    BytesReader r;
    bytes_reader_reset(&r, BYTES(in));
    CHECK(errors_is(
        binary_read(none, bytes_reader_as_io_reader(&r), binary_big_endian, data),
        burrow_err_out_of_memory));

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 100; i++)
        v[i] = i * 1000003;
    BytesBuffer buf = BYTES_BUFFER(a);
    CHECK(BURROW_OK(
        binary_write(a, bytes_buffer_as_io_writer(&buf), binary_little_endian, data)));
    int32_t back[100] = {0};
    I32Slice bsl = slice_from(back, 100, 100, TYPE_INT32);
    bytes_reader_reset(&r, bytes_buffer_bytes(&buf));
    CHECK(BURROW_OK(binary_read(a, bytes_reader_as_io_reader(&r), binary_little_endian,
                                BURROW_ANY(TYPE_OF(I32Slice), &bsl))));
    CHECK(memcmp(v, back, sizeof v) == 0);
    arena_free(&ar);
}

static void TestTooSmall(TestingT *t) {
    int64_t v = 1;
    Byte b[4];
    Error err;
    CHECK(binary_encode(BYTES(b), binary_big_endian, BURROW_ANY(TYPE_INT64, &v),
                        &err) == 0);
    CHECK(str_eq(error_text(err), BURROW_S("buffer too small")));
    CHECK(binary_decode(BYTES(b), binary_big_endian, BURROW_ANY(TYPE_INT64, &v),
                        &err) == 0);
    CHECK(str_eq(error_text(err), BURROW_S("buffer too small")));
    CHECK(v == 1);
}

/* The byte reader adapters on the three readers that have ReadByte. */
static void TestByteReaders(TestingT *t) {
    static Byte enc[] = {0xac, 0x02};
    Error err;
    BytesReader br;
    bytes_reader_reset(&br, BYTES(enc));
    CHECK(binary_read_uvarint(bytes_reader_as_io_byte_reader(&br), &err) == 300 &&
          BURROW_OK(err));
    BytesBuffer bb = BYTES_BUFFER(NULL);
    bytes_buffer_reset(&bb);
    Arena ar;
    arena_init(&ar, NULL, 0);
    bb = BYTES_BUFFER(arena_allocator(&ar));
    bytes_buffer_write(&bb, BYTES(enc), NULL);
    CHECK(binary_read_uvarint(bytes_buffer_as_io_byte_reader(&bb), &err) == 300 &&
          BURROW_OK(err));
    StringsReader sr;
    strings_reader_reset(&sr, str_from_bytes(enc, 1));
    CHECK(binary_read_uvarint(strings_reader_as_io_byte_reader(&sr), &err) == 0x2c &&
          same_error(err, io_err_unexpected_eof));
    arena_free(&ar);
}

/* ------------------------------------------------------------ benchmarks */

typedef struct SliceReader {
    Slice remain;
} SliceReader;

static Int slice_reader_read(void *self, Slice p, Error *err) {
    SliceReader *r = (SliceReader *)self;
    Int n = p.len < r->remain.len ? p.len : r->remain.len;
    memcpy(p.p, r->remain.p, (size_t)n);
    r->remain = slice_sub(r->remain, n, r->remain.len);
    *err = BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT slice_reader_vt = {NULL, slice_reader_read};

static Int discard_write(void *self, Slice p, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT discard_vt = {NULL, discard_write};

static void BenchmarkReadSlice1000Int32s(TestingB *b) {
    static int32_t v[1000];
    static Byte buf[4000];
    SliceReader bsr;
    IoReader r = {&slice_reader_vt, &bsr};
    I32Slice slice = slice_from(v, 1000, 1000, TYPE_INT32);
    Any data = BURROW_ANY(TYPE_OF(I32Slice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_b_set_bytes(b, sizeof buf);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bsr.remain = BYTES(buf);
        binary_read(a, r, binary_big_endian, data);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkReadStruct(TestingB *b) {
    init_s();
    SliceReader bsr;
    IoReader r = {&slice_reader_vt, &bsr};
    Struct t;
    StructPtr p = &t;
    Any data = BURROW_ANY(TYPE_OF(StructPtr), &p);
    testing_b_set_bytes(b, (int64_t)sizeof big);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bsr.remain = BYTES(big);
        binary_read(NULL, r, binary_big_endian, data);
    }
    testing_b_stop_timer(b);
    if (testing_b_n(b) > 0 && !struct_eq(TYPE_OF(Struct), &s, &t))
        testing_b_fatalf_v(b, "struct doesn't match");
}

static void BenchmarkWriteStruct(TestingB *b) {
    init_s();
    StructPtr p = &s;
    Any data = BURROW_ANY(TYPE_OF(StructPtr), &p);
    testing_b_set_bytes(b, binary_size(data));
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_write(NULL, (IoWriter){&discard_vt, NULL}, binary_big_endian, data);
}

static void BenchmarkAppendStruct(TestingB *b) {
    init_s();
    StructPtr p = &s;
    Any data = BURROW_ANY(TYPE_OF(StructPtr), &p);
    Byte room[128];
    Slice buf = slice_from(room, 0, binary_size(data), TYPE_BYTE);
    testing_b_set_bytes(b, buf.cap);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_encode(buf, binary_big_endian, data, NULL);
}

static void BenchmarkWriteSlice1000Structs(TestingB *b) {
    static Struct v[1000];
    StructSlice slice = slice_from(v, 1000, 1000, TYPE_OF(Struct));
    Any data = BURROW_ANY(TYPE_OF(StructSlice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    testing_b_set_bytes(b, binary_size(data));
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bytes_buffer_reset(&buf);
        binary_write(a, w, binary_big_endian, data);
    }
    testing_b_stop_timer(b);
    arena_free(&ar);
}

static void BenchmarkAppendSlice1000Structs(TestingB *b) {
    static Struct v[1000];
    StructSlice slice = slice_from(v, 1000, 1000, TYPE_OF(Struct));
    Any data = BURROW_ANY(TYPE_OF(StructSlice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = slice_make(a, TYPE_BYTE, 0, binary_size(data));
    testing_b_set_bytes(b, buf.cap);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_append(a, buf, binary_big_endian, data, NULL);
    testing_b_stop_timer(b);
    arena_free(&ar);
}

static void BenchmarkReadSlice1000Structs(TestingB *b) {
    static Struct v[1000];
    StructSlice slice = slice_from(v, 1000, 1000, TYPE_OF(Struct));
    Any data = BURROW_ANY(TYPE_OF(StructSlice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = binary_size(data);
    Slice buf = slice_make(a, TYPE_BYTE, n, n);
    SliceReader bsr;
    IoReader r = {&slice_reader_vt, &bsr};
    ArenaMark m = arena_mark(&ar);
    testing_b_set_bytes(b, n);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bsr.remain = buf;
        binary_read(a, r, binary_big_endian, data);
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

static void BenchmarkReadInts(TestingB *b) {
    init_s();
    Struct ls;
    memset(&ls, 0, sizeof ls);
    SliceReader bsr;
    IoReader r = {&slice_reader_vt, &bsr};
    testing_b_set_bytes(b, 2 * (1 + 2 + 4 + 8));
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bsr.remain = BYTES(big);
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_INT8, &ls.Int8));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_INT16, &ls.Int16));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_INT32, &ls.Int32));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_INT64, &ls.Int64));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_UINT8, &ls.Uint8));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_UINT16, &ls.Uint16));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_UINT32, &ls.Uint32));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_UINT64, &ls.Uint64));
    }
    testing_b_stop_timer(b);
    if (testing_b_n(b) > 0 && (ls.Int64 != s.Int64 || ls.Uint64 != s.Uint64))
        testing_b_fatalf_v(b, "struct doesn't match");
}

static void BenchmarkWriteInts(TestingB *b) {
    init_s();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    testing_b_set_bytes(b, 2 * (1 + 2 + 4 + 8));
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bytes_buffer_reset(&buf);
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_INT8, &s.Int8));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_INT16, &s.Int16));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_INT32, &s.Int32));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_INT64, &s.Int64));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_UINT8, &s.Uint8));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_UINT16, &s.Uint16));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_UINT32, &s.Uint32));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_UINT64, &s.Uint64));
    }
    testing_b_stop_timer(b);
    if (testing_b_n(b) > 0 &&
        !bytes_eq(bytes_buffer_bytes(&buf),
                  slice_from((void *)(uintptr_t)big, 30, 30, TYPE_BYTE)))
        testing_b_fatalf_v(b, "first half doesn't match");
    arena_free(&ar);
}

static void BenchmarkAppendInts(TestingB *b) {
    init_s();
    Byte room[256];
    Slice buf = slice_from(room, 0, 256, TYPE_BYTE);
    testing_b_set_bytes(b, 2 * (1 + 2 + 4 + 8));
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        buf = slice_sub(buf, 0, 0);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_INT8, &s.Int8), NULL);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_INT16, &s.Int16), NULL);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_INT32, &s.Int32), NULL);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_INT64, &s.Int64), NULL);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_UINT8, &s.Uint8), NULL);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_UINT16, &s.Uint16), NULL);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_UINT32, &s.Uint32), NULL);
        buf = binary_append(NULL, buf, binary_big_endian,
                            BURROW_ANY(TYPE_UINT64, &s.Uint64), NULL);
    }
    testing_b_stop_timer(b);
    if (testing_b_n(b) > 0 &&
        !bytes_eq(buf, slice_from((void *)(uintptr_t)big, 30, 30, TYPE_BYTE)))
        testing_b_fatalf_v(b, "first half doesn't match");
}

static void BenchmarkWriteSlice1000Int32s(TestingB *b) {
    static int32_t v[1000];
    I32Slice slice = slice_from(v, 1000, 1000, TYPE_INT32);
    Any data = BURROW_ANY(TYPE_OF(I32Slice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    ArenaMark m = arena_mark(&ar);
    testing_b_set_bytes(b, 4 * 1000);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bytes_buffer_reset(&buf);
        binary_write(a, w, binary_big_endian, data);
        (void)m;
    }
    testing_b_stop_timer(b);
    arena_free(&ar);
}

static void BenchmarkAppendSlice1000Int32s(TestingB *b) {
    static int32_t v[1000];
    static Byte room[4000];
    I32Slice slice = slice_from(v, 1000, 1000, TYPE_INT32);
    Any data = BURROW_ANY(TYPE_OF(I32Slice), &slice);
    Slice buf = slice_from(room, 0, 4000, TYPE_BYTE);
    testing_b_set_bytes(b, buf.cap);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_append(NULL, buf, binary_big_endian, data, NULL);
}

static Byte putbuf[8];

static void BenchmarkPutUint16(TestingB *b) {
    testing_b_set_bytes(b, 2);
    Slice p = slice_from(putbuf, 2, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_big_endian_put_uint16(p, (uint16_t)i);
}

static void BenchmarkAppendUint16(TestingB *b) {
    testing_b_set_bytes(b, 2);
    Slice p = slice_from(putbuf, 0, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        p = binary_big_endian_append_uint16(NULL, slice_sub(p, 0, 0), (uint16_t)i);
}

static void BenchmarkPutUint32(TestingB *b) {
    testing_b_set_bytes(b, 4);
    Slice p = slice_from(putbuf, 4, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_big_endian_put_uint32(p, (uint32_t)i);
}

static void BenchmarkAppendUint32(TestingB *b) {
    testing_b_set_bytes(b, 4);
    Slice p = slice_from(putbuf, 0, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        p = binary_big_endian_append_uint32(NULL, slice_sub(p, 0, 0), (uint32_t)i);
}

static void BenchmarkPutUint64(TestingB *b) {
    testing_b_set_bytes(b, 8);
    Slice p = slice_from(putbuf, 8, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_big_endian_put_uint64(p, (uint64_t)i);
}

static void BenchmarkAppendUint64(TestingB *b) {
    testing_b_set_bytes(b, 8);
    Slice p = slice_from(putbuf, 0, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        p = binary_big_endian_append_uint64(NULL, slice_sub(p, 0, 0), (uint64_t)i);
}

static void BenchmarkLittleEndianPutUint16(TestingB *b) {
    testing_b_set_bytes(b, 2);
    Slice p = slice_from(putbuf, 2, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_little_endian_put_uint16(p, (uint16_t)i);
}

static void BenchmarkLittleEndianAppendUint16(TestingB *b) {
    testing_b_set_bytes(b, 2);
    Slice p = slice_from(putbuf, 0, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        p = binary_little_endian_append_uint16(NULL, slice_sub(p, 0, 0), (uint16_t)i);
}

static void BenchmarkLittleEndianPutUint32(TestingB *b) {
    testing_b_set_bytes(b, 4);
    Slice p = slice_from(putbuf, 4, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_little_endian_put_uint32(p, (uint32_t)i);
}

static void BenchmarkLittleEndianAppendUint32(TestingB *b) {
    testing_b_set_bytes(b, 4);
    Slice p = slice_from(putbuf, 0, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        p = binary_little_endian_append_uint32(NULL, slice_sub(p, 0, 0), (uint32_t)i);
}

static void BenchmarkLittleEndianPutUint64(TestingB *b) {
    testing_b_set_bytes(b, 8);
    Slice p = slice_from(putbuf, 8, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        binary_little_endian_put_uint64(p, (uint64_t)i);
}

static void BenchmarkLittleEndianAppendUint64(TestingB *b) {
    testing_b_set_bytes(b, 8);
    Slice p = slice_from(putbuf, 0, 8, TYPE_BYTE);
    for (Int i = 0; i < testing_b_n(b); i++)
        p = binary_little_endian_append_uint64(NULL, slice_sub(p, 0, 0), (uint64_t)i);
}

static void BenchmarkReadFloats(TestingB *b) {
    init_s();
    Struct ls;
    memset(&ls, 0, sizeof ls);
    SliceReader bsr;
    IoReader r = {&slice_reader_vt, &bsr};
    testing_b_set_bytes(b, 4 + 8);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bsr.remain = slice_sub(BYTES(big), 30, (Int)sizeof big);
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_FLOAT32, &ls.Float32));
        binary_read(NULL, r, binary_big_endian, BURROW_ANY(TYPE_FLOAT64, &ls.Float64));
    }
    testing_b_stop_timer(b);
    if (testing_b_n(b) > 0 && (ls.Float32 != s.Float32 || ls.Float64 != s.Float64))
        testing_b_fatalf_v(b, "struct doesn't match");
}

static void BenchmarkWriteFloats(TestingB *b) {
    init_s();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    testing_b_set_bytes(b, 4 + 8);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bytes_buffer_reset(&buf);
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_FLOAT32, &s.Float32));
        binary_write(a, w, binary_big_endian, BURROW_ANY(TYPE_FLOAT64, &s.Float64));
    }
    testing_b_stop_timer(b);
    if (testing_b_n(b) > 0 &&
        !bytes_eq(bytes_buffer_bytes(&buf), slice_sub(BYTES(big), 30, 30 + 4 + 8)))
        testing_b_fatalf_v(b, "first half doesn't match");
    arena_free(&ar);
}

static void BenchmarkReadSlice1000Float32s(TestingB *b) {
    static float v[1000];
    static Byte buf[4000];
    SliceReader bsr;
    IoReader r = {&slice_reader_vt, &bsr};
    F32Slice slice = slice_from(v, 1000, 1000, TYPE_FLOAT32);
    Any data = BURROW_ANY(TYPE_OF(F32Slice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_b_set_bytes(b, sizeof buf);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bsr.remain = BYTES(buf);
        binary_read(a, r, binary_big_endian, data);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkWriteSlice1000Float32s(TestingB *b) {
    static float v[1000];
    F32Slice slice = slice_from(v, 1000, 1000, TYPE_FLOAT32);
    Any data = BURROW_ANY(TYPE_OF(F32Slice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    testing_b_set_bytes(b, 4 * 1000);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bytes_buffer_reset(&buf);
        binary_write(a, w, binary_big_endian, data);
    }
    testing_b_stop_timer(b);
    arena_free(&ar);
}

static void BenchmarkReadSlice1000Uint8s(TestingB *b) {
    static uint8_t v[1000];
    static Byte buf[1000];
    SliceReader bsr;
    IoReader r = {&slice_reader_vt, &bsr};
    U8Slice slice = slice_from(v, 1000, 1000, TYPE_UINT8);
    Any data = BURROW_ANY(TYPE_OF(U8Slice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_b_set_bytes(b, sizeof buf);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bsr.remain = BYTES(buf);
        binary_read(a, r, binary_big_endian, data);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkWriteSlice1000Uint8s(TestingB *b) {
    static uint8_t v[1000];
    U8Slice slice = slice_from(v, 1000, 1000, TYPE_UINT8);
    Any data = BURROW_ANY(TYPE_OF(U8Slice), &slice);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    testing_b_set_bytes(b, 1000);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        bytes_buffer_reset(&buf);
        binary_write(a, w, binary_big_endian, data);
    }
    arena_free(&ar);
}

static void BenchmarkPutUvarint32(TestingB *b) {
    Byte room[BINARY_MAX_VARINT_LEN32];
    Slice buf = slice_from(room, sizeof room, sizeof room, TYPE_BYTE);
    testing_b_set_bytes(b, 4);
    for (Int i = 0; i < testing_b_n(b); i++)
        for (unsigned j = 0; j < BINARY_MAX_VARINT_LEN32; j++)
            binary_put_uvarint(buf, (uint64_t)1 << (j * 7));
}

static void BenchmarkPutUvarint64(TestingB *b) {
    Byte room[BINARY_MAX_VARINT_LEN64];
    Slice buf = slice_from(room, sizeof room, sizeof room, TYPE_BYTE);
    testing_b_set_bytes(b, 8);
    for (Int i = 0; i < testing_b_n(b); i++)
        for (unsigned j = 0; j < BINARY_MAX_VARINT_LEN64; j++)
            binary_put_uvarint(buf, (uint64_t)1 << (j * 7));
}

#define TESTS(X)                                                                       \
    X(TestLittleEndianRead)                                                            \
    X(TestLittleEndianWrite)                                                           \
    X(TestLittleEndianPtrWrite)                                                        \
    X(TestBigEndianRead)                                                               \
    X(TestBigEndianWrite)                                                              \
    X(TestBigEndianPtrWrite)                                                           \
    X(TestReadSlice)                                                                   \
    X(TestWriteSlice)                                                                  \
    X(TestReadBool)                                                                    \
    X(TestReadBoolSlice)                                                               \
    X(TestSliceRoundTrip)                                                              \
    X(TestWriteT)                                                                      \
    X(TestBlankFields)                                                                 \
    X(TestSizeInvalid)                                                                 \
    X(TestUnexportedRead)                                                              \
    X(TestReadErrorMsg)                                                                \
    X(TestReadTruncated)                                                               \
    X(TestByteOrder)                                                                   \
    X(TestEarlyBoundsChecks)                                                           \
    X(TestNoFixedSize)                                                                 \
    X(TestAppendNoAlloc)                                                               \
    X(TestNativeEndian)                                                                \
    X(TestConstants)                                                                   \
    X(TestVarint)                                                                      \
    X(TestUvarint)                                                                     \
    X(TestBufferTooSmall)                                                              \
    X(TestBufferTooBigWithOverflow)                                                    \
    X(TestOverflow)                                                                    \
    X(TestNonCanonicalZero)                                                            \
    X(TestPanics)                                                                      \
    X(TestNilStructPointer)                                                            \
    X(TestPaddedAppend)                                                                \
    X(TestOrderNames)                                                                  \
    X(TestCustomOrder)                                                                 \
    X(TestWriteBytes)                                                                  \
    X(TestBigValue)                                                                    \
    X(TestTooSmall)                                                                    \
    X(TestByteReaders)                                                                 \
    X(BenchmarkReadSlice1000Int32s)                                                    \
    X(BenchmarkReadStruct)                                                             \
    X(BenchmarkWriteStruct)                                                            \
    X(BenchmarkAppendStruct)                                                           \
    X(BenchmarkWriteSlice1000Structs)                                                  \
    X(BenchmarkAppendSlice1000Structs)                                                 \
    X(BenchmarkReadSlice1000Structs)                                                   \
    X(BenchmarkReadInts)                                                               \
    X(BenchmarkWriteInts)                                                              \
    X(BenchmarkAppendInts)                                                             \
    X(BenchmarkWriteSlice1000Int32s)                                                   \
    X(BenchmarkAppendSlice1000Int32s)                                                  \
    X(BenchmarkPutUint16)                                                              \
    X(BenchmarkAppendUint16)                                                           \
    X(BenchmarkPutUint32)                                                              \
    X(BenchmarkAppendUint32)                                                           \
    X(BenchmarkPutUint64)                                                              \
    X(BenchmarkAppendUint64)                                                           \
    X(BenchmarkLittleEndianPutUint16)                                                  \
    X(BenchmarkLittleEndianAppendUint16)                                               \
    X(BenchmarkLittleEndianPutUint32)                                                  \
    X(BenchmarkLittleEndianAppendUint32)                                               \
    X(BenchmarkLittleEndianPutUint64)                                                  \
    X(BenchmarkLittleEndianAppendUint64)                                               \
    X(BenchmarkReadFloats)                                                             \
    X(BenchmarkWriteFloats)                                                            \
    X(BenchmarkReadSlice1000Float32s)                                                  \
    X(BenchmarkWriteSlice1000Float32s)                                                 \
    X(BenchmarkReadSlice1000Uint8s)                                                    \
    X(BenchmarkWriteSlice1000Uint8s)                                                   \
    X(BenchmarkPutUvarint32)                                                           \
    X(BenchmarkPutUvarint64)

TESTING_MAIN(TESTS)
