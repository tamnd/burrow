/* Tests for reflect, the half that reads.
 *
 * These are Go's tests from all_test.go, iter_test.go and visiblefields_test.go
 * for what this half has: kinds, the Type questions, Value's getters, fields
 * found by index and by name, maps and their iterators, Seq and Seq2, Equal,
 * Comparable, IsNil, IsZero, Swapper and DeepEqual. Each keeps Go's name and
 * the cases of Go's table that need nothing from the half that writes, which
 * is Set, New, Zero, MakeFunc, Call and the type constructors. Those tests
 * arrive with that half.
 *
 * Go writes its types inline. C cannot, so the types are declared up front,
 * with BURROW_STRUCT where the type has a name and by hand where it does not
 * or where the test needs it to belong to package reflect_test.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/reflect.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* A descriptor written out, for the types the declare macros do not make. */
#define TYPE_DESC(name, pkg, kind, C, nfield, fields, elem, key, len)                  \
    {                                                                                  \
        name,                                                                          \
        pkg,                                                                           \
        kind,                                                                          \
        (uint32_t)sizeof(C),                                                           \
        (uint16_t)_Alignof(C),                                                         \
        nfield,                                                                        \
        0,                                                                             \
        fields,                                                                        \
        NULL,                                                                          \
        elem,                                                                          \
        key,                                                                           \
        len,                                                                           \
        0,                                                                             \
        NULL,                                                                          \
    }

#define NONAME {NULL, 0}

static Str cs(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

static ReflectValue val(const Type *t, const void *p) {
    return reflect_value_of(BURROW_ANY(t, (void *)(uintptr_t)p));
}

static Slice ints_of(Int *p, Int n) {
    return slice_from(p, n, n, TYPE_INT);
}

/* Runs f(env) and copies the text of its panic, if there was one, into buf. */
static bool panics(void (*f)(void *), void *env, char *buf, size_t n) {
    volatile bool got = false;
    buf[0] = 0;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        Str text = panic_text(r);
        size_t k = (size_t)text.len < n - 1 ? (size_t)text.len : n - 1;
        memcpy(buf, text.p, k);
        buf[k] = 0;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* ------------------------------------------------------------------ types */

static const Type type_I =
    TYPE_DESC(S_("I"), S_("reflect_test"), KIND_INT, Int, 0, NULL, NULL, NULL, 0);
static const Type type_ptr_I =
    TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0, NULL, &type_I, NULL, 0);
static const Type type_S =
    TYPE_DESC(S_("S"), S_("reflect_test"), KIND_STRING, Str, 0, NULL, NULL, NULL, 0);
static const Type type_MyByte = TYPE_DESC(S_("MyByte"), S_("reflect_test"), KIND_UINT8,
                                          uint8_t, 0, NULL, NULL, NULL, 0);

BURROW_SLICE_TYPE(Ints, Int);
BURROW_SLICE_TYPE(Uintptrs, Uintptr);
BURROW_SLICE_TYPE(Int16s, int16_t);
BURROW_SLICE_TYPE(Int8s, int8_t);
BURROW_SLICE_TYPE(Strs, Str);
BURROW_SLICE_TYPE(U8s, uint8_t);
BURROW_SLICE_TYPE(Float64s, double);
BURROW_SLICE_TYPE(IntsSlice, Ints);
BURROW_MAP_TYPE(StrIntMap, Str, Int);
BURROW_MAP_TYPE(IntStrMap, Int, Str);
BURROW_MAP_TYPE(UintStrMap, Uint, Str);
BURROW_MAP_TYPE(IntIntMap, Int, Int);
BURROW_ARRAY_TYPE(Int3, Int, 3);
BURROW_ARRAY_TYPE(Int4, Int, 4);
BURROW_ARRAY_TYPE(Byte8, uint8_t, 8);
BURROW_ARRAY_TYPE(Str5, Str, 5);
BURROW_ARRAY_TYPE(Float1, double, 1);
BURROW_ARRAY_TYPE(Ints3, Ints, 3);
BURROW_PTR_TYPE(Int3Ptr, Int3);
BURROW_PTR_TYPE(Int4Ptr, Int4);
BURROW_PTR_TYPE(Float1Ptr, Float1);
BURROW_PTR_TYPE(IntPtr, Int);
BURROW_PTR_TYPE(StrPtr, Str);

static const Type type_ptrs_I =
    TYPE_DESC(NONAME, NONAME, KIND_SLICE, Slice, 0, NULL, &type_ptr_I, NULL, 0);
static const Type type_Ss =
    TYPE_DESC(NONAME, NONAME, KIND_SLICE, Slice, 0, NULL, &type_S, NULL, 0);
static const Type type_MyBytes_unnamed =
    TYPE_DESC(NONAME, NONAME, KIND_SLICE, Slice, 0, NULL, &type_MyByte, NULL, 0);
static const Type type_MyBytes =
    TYPE_DESC(S_("MyBytes"), S_("reflect_test"), KIND_SLICE, Slice, 0, NULL,
              &type_MyByte, NULL, 0);
static const Type type_A4 = TYPE_DESC(S_("A"), S_("reflect_test"), KIND_ARRAY, Byte8, 0,
                                      NULL, TYPE_UINT8, NULL, 4);
static const Type type_ptr_A4 =
    TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0, NULL, &type_A4, NULL, 0);
static const Type type_chan_int =
    TYPE_DESC(NONAME, NONAME, KIND_CHAN, void *, 0, NULL, TYPE_INT, NULL, 0);
static const Type type_func =
    TYPE_DESC(NONAME, NONAME, KIND_FUNC, Func, 0, NULL, NULL, NULL, 0);

/* Go's pair, pairPtr and the rest of TestSwapper's. */
#define PAIR_FIELDS(F, T)                                                              \
    F(T, Int, x, "")                                                                   \
    F(T, Int, y, "")
BURROW_STRUCT(Pair, PAIR_FIELDS);
BURROW_SLICE_TYPE(Pairs, Pair);

/* Basic and NotBasic, which have the same fields and different names. */
#define BASIC_FIELDS(F, T)                                                             \
    F(T, Int, x, "")                                                                   \
    F(T, float, y, "")
BURROW_STRUCT(Basic, BASIC_FIELDS);
BURROW_STRUCT(NotBasic, BASIC_FIELDS);

/* Recursive, from TestDeepEqualRecursiveStruct. */
typedef struct Recursive *RecursivePtr;
extern const Type burrow_type_RecursivePtr;
#define RECURSIVE_FIELDS(F, T)                                                         \
    F(T, Int, x, "")                                                                   \
    F(T, RecursivePtr, r, "")
BURROW_STRUCT(Recursive, RECURSIVE_FIELDS);
const Type burrow_type_RecursivePtr = TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0,
                                                NULL, &burrow_type_Recursive, NULL, 0);

/* structWithSelfPtr. */
typedef struct SelfPtr *SelfPtrPtr;
extern const Type burrow_type_SelfPtrPtr;
#define SELFPTR_FIELDS(F, T)                                                           \
    F(T, SelfPtrPtr, p, "")                                                            \
    F(T, Str, s, "")
BURROW_STRUCT(SelfPtr, SELFPTR_FIELDS);
const Type burrow_type_SelfPtrPtr = TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0,
                                              NULL, &burrow_type_SelfPtr, NULL, 0);

/* type Loop *Loop. */
static const Type type_Loop = TYPE_DESC(S_("Loop"), S_("reflect_test"), KIND_POINTER,
                                        void *, 0, NULL, &type_Loop, NULL, 0);
static const Type type_ptr_Loop =
    TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0, NULL, &type_Loop, NULL, 0);

/* struct{ x any }, and the like, for TestIsNil and TestInvalid. */
#define WITHANY_FIELDS(F, T) F(T, Any, v, "")
BURROW_STRUCT(WithAny, WITHANY_FIELDS);

/* Impl, for TestValueString. */
static const Field impl_fields[] = {{S_("X"), NONAME, &burrow_type_Int, 0}};
static const Type type_Impl = TYPE_DESC(S_("Impl"), S_("reflect_test"), KIND_STRUCT,
                                        Int, 1, impl_fields, NULL, NULL, 0);

/* struct{}. */
static const Type type_empty = {NONAME, NONAME, KIND_STRUCT, 0,    1, 0, 0,
                                NULL,   NULL,   NULL,        NULL, 0, 0, NULL};
static Byte empty_value;

/* --------------------------------------------------- fieldTests' types */

#define D1_FIELDS(F, T) F(T, Int, d, "")
BURROW_STRUCT(D1, D1_FIELDS);
BURROW_STRUCT(D2, D1_FIELDS);

#define S0_FIELDS(F, T)                                                                \
    F(T, Int, A, "")                                                                   \
    F(T, Int, B, "")                                                                   \
    F(T, Int, C, "")                                                                   \
    F(T, D1, D1, "")                                                                   \
    F(T, D2, D2, "")
BURROW_STRUCT(S0, S0_FIELDS);

#define S1_FIELDS(F, T)                                                                \
    F(T, Int, B, "")                                                                   \
    F(T, S0, S0, "")
BURROW_STRUCT(S1, S1_FIELDS);
BURROW_PTR_TYPE(S1Ptr, S1);

#define S2_FIELDS(F, T)                                                                \
    F(T, Int, A, "")                                                                   \
    F(T, S1Ptr, S1, "")
BURROW_STRUCT(S2, S2_FIELDS);

#define S1X_FIELDS(F, T) F(T, S1, S1, "")
BURROW_STRUCT(S1x, S1X_FIELDS);
BURROW_STRUCT(S1y, S1X_FIELDS);
BURROW_PTR_TYPE(S1yPtr, S1y);

#define S3_FIELDS(F, T)                                                                \
    F(T, S1x, S1x, "")                                                                 \
    F(T, S2, S2, "")                                                                   \
    F(T, Int, D, "")                                                                   \
    F(T, Int, E, "")                                                                   \
    F(T, S1yPtr, S1y, "")
BURROW_STRUCT(S3, S3_FIELDS);

typedef struct S4 *S4Ptr;
extern const Type burrow_type_S4Ptr;
#define S4_FIELDS(F, T)                                                                \
    F(T, S4Ptr, S4, "")                                                                \
    F(T, Int, A, "")
BURROW_STRUCT(S4, S4_FIELDS);
const Type burrow_type_S4Ptr =
    TYPE_DESC(NONAME, NONAME, KIND_POINTER, void *, 0, NULL, &burrow_type_S4, NULL, 0);

#define S6_FIELDS(F, T) F(T, Int, X, "")
BURROW_STRUCT(S6, S6_FIELDS);
BURROW_STRUCT(S7, S6_FIELDS);

#define S9_FIELDS(F, T)                                                                \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT(S9, S9_FIELDS);

#define S8_FIELDS(F, T) F(T, S9, S9, "")
BURROW_STRUCT(S8, S8_FIELDS);

#define S5_FIELDS(F, T)                                                                \
    F(T, S6, S6, "")                                                                   \
    F(T, S7, S7, "")                                                                   \
    F(T, S8, S8, "")
BURROW_STRUCT(S5, S5_FIELDS);

#define S11_FIELDS(F, T) F(T, S6, S6, "")
BURROW_STRUCT(S11, S11_FIELDS);
BURROW_STRUCT(S12, S11_FIELDS);

#define S13_FIELDS(F, T) F(T, S8, S8, "")
BURROW_STRUCT(S13, S13_FIELDS);

#define S10_FIELDS(F, T)                                                               \
    F(T, S11, S11, "")                                                                 \
    F(T, S12, S12, "")                                                                 \
    F(T, S13, S13, "")
BURROW_STRUCT(S10, S10_FIELDS);

#define S15_FIELDS(F, T) F(T, S11, S11, "")
BURROW_STRUCT(S15, S15_FIELDS);
BURROW_STRUCT(S16, S15_FIELDS);

#define S14_FIELDS(F, T)                                                               \
    F(T, S15, S15, "")                                                                 \
    F(T, S16, S16, "")
BURROW_STRUCT(S14, S14_FIELDS);

/* ------------------------------------------------------------- kinds */

static void TestNegativeKindString(TestingT *t) {
    Str s = reflect_kind_string((ReflectKind)-1);
    if (!str_eq(s, BURROW_S("kind-1")))
        testing_t_fatalf_v(t, "Kind(-1).String() = %q, want %q", s, "kind-1");
}

/* ------------------------------------------------------- fieldTests */

typedef struct FTest {
    const Type *type;
    const void *s;
    const char *name;
    Int index[4];
    Int nindex;
    Int value;
} FTest;

static S0 ft_s0_a = {.A = 'a'};
static S0 ft_s0;
static S1 ft_s1_a = {.S0 = {.A = 'a'}};
static S1 ft_s1_b = {.B = 'b'};
static S1 ft_s1;
static S1 ft_s1_c = {.S0 = {.C = 'c'}};
static S2 ft_s2_a = {.A = 'a'};
static S2 ft_s2;
static S1 ft_s1_b2 = {.B = 'b'};
static S2 ft_s2_b = {.S1 = &ft_s1_b2};
static S1 ft_s1_c2 = {.S0 = {.C = 'c'}};
static S2 ft_s2_c = {.S1 = &ft_s1_c2};
static S3 ft_s3;
static S3 ft_s3_a = {.S2 = {.A = 'a'}};
static S3 ft_s3_d = {.D = 'd'};
static S3 ft_s3_e = {.E = 'e'};
static S4 ft_s4_a = {.A = 'a'};
static S4 ft_s4;
static S5 ft_s5;
static S10 ft_s10;
static S14 ft_s14;

static const FTest field_tests[] = {
    {&type_empty, &empty_value, "", {0}, 0, 0},
    {&type_empty, &empty_value, "Foo", {0}, 0, 0},
    {&burrow_type_S0, &ft_s0_a, "A", {0}, 1, 'a'},
    {&burrow_type_S0, &ft_s0, "D", {0}, 0, 0},
    {&burrow_type_S1, &ft_s1_a, "A", {1, 0}, 2, 'a'},
    {&burrow_type_S1, &ft_s1_b, "B", {0}, 1, 'b'},
    {&burrow_type_S1, &ft_s1, "S0", {1}, 1, 0},
    {&burrow_type_S1, &ft_s1_c, "C", {1, 2}, 2, 'c'},
    {&burrow_type_S2, &ft_s2_a, "A", {0}, 1, 'a'},
    {&burrow_type_S2, &ft_s2, "S1", {1}, 1, 0},
    {&burrow_type_S2, &ft_s2_b, "B", {1, 0}, 2, 'b'},
    {&burrow_type_S2, &ft_s2_c, "C", {1, 1, 2}, 3, 'c'},
    {&burrow_type_S2, &ft_s2, "D", {0}, 0, 0},
    {&burrow_type_S3, &ft_s3, "S1", {0}, 0, 0},
    {&burrow_type_S3, &ft_s3_a, "A", {1, 0}, 2, 'a'},
    {&burrow_type_S3, &ft_s3, "B", {0}, 0, 0},
    {&burrow_type_S3, &ft_s3_d, "D", {2}, 1, 0},
    {&burrow_type_S3, &ft_s3_e, "E", {3}, 1, 'e'},
    {&burrow_type_S4, &ft_s4_a, "A", {1}, 1, 'a'},
    {&burrow_type_S4, &ft_s4, "B", {0}, 0, 0},
    {&burrow_type_S5, &ft_s5, "X", {0}, 0, 0},
    {&burrow_type_S5, &ft_s5, "Y", {2, 0, 1}, 3, 0},
    {&burrow_type_S10, &ft_s10, "X", {0}, 0, 0},
    {&burrow_type_S10, &ft_s10, "Y", {2, 0, 0, 1}, 4, 0},
    {&burrow_type_S14, &ft_s14, "X", {0}, 0, 0},
};

static Slice ft_index(const FTest *ft) {
    if (ft->nindex == 0)
        return slice_nil(TYPE_INT);
    return slice_from((void *)(uintptr_t)ft->index, ft->nindex, ft->nindex, TYPE_INT);
}

static void check_int_value(TestingT *t, const FTest *ft, ReflectValue v) {
    if (!reflect_value_is_valid(v)) {
        testing_t_errorf_v(t, "%q.%s value not found", reflect_type_string(ft->type),
                           ft->name);
        return;
    }
    Any x = reflect_value_interface(v);
    if (x.t != TYPE_INT) {
        testing_t_errorf_v(t, "%q.%s value not an int", reflect_type_string(ft->type),
                           ft->name);
        return;
    }
    if (*(const Int *)x.data != ft->value)
        testing_t_errorf_v(t, "%q.%s is %d; want %d", reflect_type_string(ft->type),
                           ft->name, *(const Int *)x.data, ft->value);
}

static void TestFieldByIndex(TestingT *t) {
    for (Int i = 0; i < NELEM(field_tests); i++) {
        const FTest *ft = &field_tests[i];
        ReflectStructField f = reflect_type_field_by_index(ft->type, ft_index(ft));
        if (f.name.len > 0) {
            if (ft->nindex > 0) {
                if (!str_eq(f.name, cs(ft->name)))
                    testing_t_errorf_v(t, "%q.%q found; want %s",
                                       reflect_type_name(ft->type), f.name, ft->name);
            } else {
                testing_t_errorf_v(t, "%q.%q found", reflect_type_name(ft->type),
                                   f.name);
            }
        } else if (ft->nindex > 0) {
            testing_t_errorf_v(t, "%q.%s not found", reflect_type_name(ft->type),
                               ft->name);
        }
        if (ft->value != 0)
            check_int_value(
                t, ft,
                reflect_value_field_by_index(val(ft->type, ft->s), ft_index(ft)));
    }
}

static void TestFieldByName(TestingT *t) {
    for (Int i = 0; i < NELEM(field_tests); i++) {
        const FTest *ft = &field_tests[i];
        Arena ar;
        arena_init(&ar, heap_allocator(), 256);
        ReflectStructField f;
        bool found = reflect_type_field_by_name(arena_allocator(&ar), ft->type,
                                                cs(ft->name), &f);
        if (found) {
            if (ft->nindex > 0) {
                if (f.index.len != ft->nindex) {
                    testing_t_errorf_v(t, "%q.%s depth %d; want %d",
                                       reflect_type_name(ft->type), ft->name,
                                       f.index.len, ft->nindex);
                } else {
                    const Int *ix = f.index.p;
                    for (Int j = 0; j < f.index.len; j++)
                        if (ix[j] != ft->index[j])
                            testing_t_errorf_v(t, "%q.%s.Index[%d] is %d; want %d",
                                               reflect_type_name(ft->type), ft->name, j,
                                               ix[j], ft->index[j]);
                }
            } else {
                testing_t_errorf_v(t, "%q.%q found", reflect_type_name(ft->type),
                                   f.name);
            }
        } else if (ft->nindex > 0) {
            testing_t_errorf_v(t, "%q.%s not found", reflect_type_name(ft->type),
                               ft->name);
        }
        arena_free(&ar);
        if (ft->value != 0)
            check_int_value(
                t, ft, reflect_value_field_by_name(val(ft->type, ft->s), cs(ft->name)));
    }
}

/* -------------------------------------------------------- VisibleFields */

#define SFG_FIELDS(X, T)                                                               \
    X(T, Int, F, "")                                                                   \
    X(T, Int, G, "")
BURROW_STRUCT(SFG, SFG_FIELDS);
BURROW_STRUCT(sFG, SFG_FIELDS);

#define SF_FIELDS(X, T) X(T, Int, F, "")
BURROW_STRUCT(SF, SF_FIELDS);

#define SIMPLE_FIELDS(F, T)                                                            \
    F(T, Int, A, "")                                                                   \
    F(T, Str, B, "")                                                                   \
    F(T, bool, C, "")
BURROW_STRUCT(Simple, SIMPLE_FIELDS);

#define INNERX_FIELDS(F, T) F(T, Int, X, "")
BURROW_STRUCT(InnerX, INNERX_FIELDS);
#define NONEMB_FIELDS(F, T) F(T, InnerX, A, "")
BURROW_STRUCT(NonEmb, NONEMB_FIELDS);

#define EMBSFG_FIELDS(F, T) F(T, SFG, SFG, "")
BURROW_STRUCT(EmbSFG, EMBSFG_FIELDS);
#define EMBSFGL_FIELDS(F, T) F(T, sFG, sFG, "")
BURROW_STRUCT(EmbsFG, EMBSFGL_FIELDS);
#define TWO_FIELDS(F, T)                                                               \
    F(T, SFG, SFG, "")                                                                 \
    F(T, SF, SF, "")
BURROW_STRUCT(TwoEmb, TWO_FIELDS);

typedef struct StructField {
    const char *name;
    Int index[4];
    Int nindex;
} StructField;

typedef struct FieldsTest {
    const char *test_name;
    const Type *type;
    StructField expect[4];
    Int nexpect;
} FieldsTest;

static const FieldsTest fields_tests[] = {
    {"SimpleStruct",
     &burrow_type_Simple,
     {{"A", {0}, 1}, {"B", {1}, 1}, {"C", {2}, 1}},
     3},
    {"NonEmbeddedStructMember", &burrow_type_NonEmb, {{"A", {0}, 1}}, 1},
    {"EmbeddedExportedStruct",
     &burrow_type_EmbSFG,
     {{"SFG", {0}, 1}, {"F", {0, 0}, 2}, {"G", {0, 1}, 2}},
     3},
    {"EmbeddedUnexportedStruct",
     &burrow_type_EmbsFG,
     {{"sFG", {0}, 1}, {"F", {0, 0}, 2}, {"G", {0, 1}, 2}},
     3},
    {"TwoEmbeddedStructsWithCancelingMembers",
     &burrow_type_TwoEmb,
     {{"SFG", {0}, 1}, {"G", {0, 1}, 2}, {"SF", {1}, 1}},
     3},
};

static bool same_field(ReflectStructField a, ReflectStructField b) {
    return str_eq(a.name, b.name) && str_eq(a.pkg_path, b.pkg_path) &&
           a.type == b.type && str_eq(a.tag, b.tag) && a.offset == b.offset &&
           a.anonymous == b.anonymous;
}

static void TestFields(TestingT *t) {
    for (Int i = 0; i < NELEM(fields_tests); i++) {
        const FieldsTest *ft = &fields_tests[i];
        Arena ar;
        arena_init(&ar, heap_allocator(), 1024);
        Alloc *a = arena_allocator(&ar);
        Slice fields = reflect_visible_fields(a, ft->type);
        if (fields.len != ft->nexpect) {
            testing_t_errorf_v(t, "%s: unexpected field count; got %d want %d",
                               ft->test_name, fields.len, ft->nexpect);
            arena_free(&ar);
            continue;
        }
        const ReflectStructField *got = fields.p;
        for (Int j = 0; j < fields.len; j++) {
            const StructField *e = &ft->expect[j];
            Slice want_index =
                slice_from((void *)(uintptr_t)e->index, e->nindex, e->nindex, TYPE_INT);
            ReflectStructField got_field =
                reflect_type_field_by_index(ft->type, got[j].index);
            ReflectStructField want_field =
                reflect_type_field_by_index(ft->type, want_index);
            if (!same_field(got_field, want_field))
                testing_t_errorf_v(t, "%s: field %d: got %q want %q", ft->test_name, j,
                                   got_field.name, want_field.name);
            ReflectStructField by_name;
            if (!reflect_type_field_by_name(a, ft->type, cs(e->name), &by_name)) {
                testing_t_errorf_v(t, "%s: field %s not accessible by name",
                                   ft->test_name, e->name);
                continue;
            }
            if (!same_field(by_name, want_field))
                testing_t_errorf_v(t, "%s: unexpected FieldByName result for %s",
                                   ft->test_name, e->name);
        }
        arena_free(&ar);
    }
}

/* FieldByIndexErr on a nil embedded pointer. */
#define FBA_FIELDS(F, T) F(T, Str, S, "")
BURROW_STRUCT(A, FBA_FIELDS);
BURROW_PTR_TYPE(APtr, A);
#define FBB_FIELDS(F, T) F(T, APtr, A, "")
BURROW_STRUCT(B, FBB_FIELDS);

static void TestFieldByIndexErr(TestingT *t) {
    B b = {NULL};
    Int path[] = {0, 0};
    Error err = BURROW_NO_ERROR;
    (void)reflect_value_field_by_index_err(val(&burrow_type_B, &b), ints_of(path, 2),
                                           &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "expected error");
    Str text = error_text(err);
    if (strings_index(text, BURROW_S("embedded struct field A")) < 0)
        testing_t_fatalf_v(t, "%q", text);
}

typedef struct FieldIndexEnv {
    const Type *t;
    Int i;
} FieldIndexEnv;

static void field_index(void *env) {
    const FieldIndexEnv *e = env;
    (void)reflect_type_field(e->t, e->i);
}

static void TestTypeFieldOutOfRangePanic(TestingT *t) {
    static const struct {
        Int i;
        bool must_panic;
    } tests[] = {{-2, true}, {0, false}, {1, true}, {1 << 10, true}};
    for (Int i = 0; i < NELEM(tests); i++) {
        FieldIndexEnv e = {&type_Impl, tests[i].i};
        char buf[128];
        bool got = panics(field_index, &e, buf, sizeof buf);
        if (tests[i].must_panic && !got)
            testing_t_errorf_v(t, "#%d: fieldIndex %d expected to panic", i,
                               tests[i].i);
        if (!tests[i].must_panic && got)
            testing_t_errorf_v(t, "#%d: got err=%s, expected no panic", i, buf);
    }
}

/* ------------------------------------------------------------ getters */

static void TestTagGet(TestingT *t) {
    static const struct {
        const char *tag, *key, *value;
    } tests[] = {
        {"protobuf:\"PB(1,2)\"", "protobuf", "PB(1,2)"},
        {"protobuf:\"PB(1,2)\"", "foo", ""},
        {"protobuf:\"PB(1,2)\"", "rotobuf", ""},
        {"protobuf:\"PB(1,2)\" json:\"name\"", "json", "name"},
        {"protobuf:\"PB(1,2)\" json:\"name\"", "protobuf", "PB(1,2)"},
        {"k0:\"values contain spaces\" k1:\"and\\ttabs\"", "k0",
         "values contain spaces"},
        {"k0:\"values contain spaces\" k1:\"and\\ttabs\"", "k1", "and\ttabs"},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 256);
        Str v = reflect_struct_tag_get(cs(tests[i].tag), arena_allocator(&ar),
                                       cs(tests[i].key));
        if (!str_eq(v, cs(tests[i].value)))
            testing_t_errorf_v(t, "StructTag(%s).Get(%s) = %q, want %s", tests[i].tag,
                               tests[i].key, v, tests[i].value);
        arena_free(&ar);
    }
}

static void TestCanIntUintFloatComplex(TestingT *t) {
    static Int i132 = 132;
    static int8_t i8 = 8;
    static int16_t i16 = 16;
    static int32_t i32 = 32;
    static int64_t i64 = 64;
    static Uint u132 = 132;
    static uint8_t u8 = 8;
    static uint16_t u16 = 16;
    static uint32_t u32 = 32;
    static uint64_t u64 = 64;
    static Uintptr up = 0xABCD;
    static float f32 = 256.25f;
    static double f64 = 512.125;
    static Complex64 c64 = {532.125f, 10};
    static Complex128 c128 = {564.25, 1};
    static Int integer = -132;
    static Str hello = S_("hello world");
    static Int *ip;
    static const struct {
        const Type *t;
        const void *p;
        bool want[4];
    } tests[] = {
        {&burrow_type_Int, &i132, {true, false, false, false}},
        {&burrow_type_int8_t, &i8, {true, false, false, false}},
        {&burrow_type_int16_t, &i16, {true, false, false, false}},
        {&burrow_type_int32_t, &i32, {true, false, false, false}},
        {&burrow_type_int64_t, &i64, {true, false, false, false}},
        {&burrow_type_Uint, &u132, {false, true, false, false}},
        {&burrow_type_uint8_t, &u8, {false, true, false, false}},
        {&burrow_type_uint16_t, &u16, {false, true, false, false}},
        {&burrow_type_uint32_t, &u32, {false, true, false, false}},
        {&burrow_type_uint64_t, &u64, {false, true, false, false}},
        {&burrow_type_Uintptr, &up, {false, true, false, false}},
        {&burrow_type_float, &f32, {false, false, true, false}},
        {&burrow_type_double, &f64, {false, false, true, false}},
        {&burrow_type_Complex64, &c64, {false, false, false, true}},
        {&burrow_type_Complex128, &c128, {false, false, false, true}},
        {&type_I, &integer, {true, false, false, false}},
        {&burrow_type_Str, &hello, {false, false, false, false}},
        {&burrow_type_IntPtr, &ip, {false, false, false, false}},
    };
    static const char *const ops[] = {"CanInt", "CanUint", "CanFloat", "CanComplex"};
    for (Int i = 0; i < NELEM(tests); i++) {
        ReflectValue v = val(tests[i].t, tests[i].p);
        bool got[4] = {reflect_value_can_int(v), reflect_value_can_uint(v),
                       reflect_value_can_float(v), reflect_value_can_complex(v)};
        for (int j = 0; j < 4; j++)
            if (got[j] != tests[i].want[j])
                testing_t_errorf_v(t, "#%d: v.%s() returned %t for type %q, want %t", i,
                                   ops[j], got[j], reflect_type_string(tests[i].t),
                                   tests[i].want[j]);
    }
}

static void TestValueOverflow(TestingT *t) {
    double f64 = 0;
    float f32 = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    if (reflect_value_overflow_float(val(TYPE_FLOAT64, &f64), 1e300))
        testing_t_errorf_v(t, "%g wrongly overflows float64", 1e300);
    double max_float32 = (double)((1LL << 24) - 1) * ldexp(1, 127 - 23);
    if (reflect_value_overflow_float(val(TYPE_FLOAT32, &f32), max_float32))
        testing_t_errorf_v(t, "%g wrongly overflows float32", max_float32);
    double ovf_float32 = max_float32 + ldexp(1, 127 - 52);
    if (!reflect_value_overflow_float(val(TYPE_FLOAT32, &f32), ovf_float32))
        testing_t_errorf_v(t, "%g should overflow float32", ovf_float32);
    if (!reflect_value_overflow_float(val(TYPE_FLOAT32, &f32), -ovf_float32))
        testing_t_errorf_v(t, "%g should overflow float32", -ovf_float32);
    if (reflect_value_overflow_int(val(TYPE_INT32, &i32), 0x7fffffff))
        testing_t_errorf_v(t, "%d wrongly overflows int32", 0x7fffffff);
    if (reflect_value_overflow_int(val(TYPE_INT32, &i32), -(1LL << 31)))
        testing_t_errorf_v(t, "%d wrongly overflows int32", -(1LL << 31));
    if (!reflect_value_overflow_int(val(TYPE_INT32, &i32), 1LL << 31))
        testing_t_errorf_v(t, "%d should overflow int32", 1LL << 31);
    if (reflect_value_overflow_uint(val(TYPE_UINT32, &u32), 0xffffffffu))
        testing_t_errorf_v(t, "%d wrongly overflows uint32", 0xffffffffu);
    if (!reflect_value_overflow_uint(val(TYPE_UINT32, &u32), 1ULL << 32))
        testing_t_errorf_v(t, "%d should overflow uint32", 1ULL << 32);
}

static void TestTypeOverflow(TestingT *t) {
    if (reflect_type_overflow_float(TYPE_FLOAT64, 1e300))
        testing_t_errorf_v(t, "%g wrongly overflows float64", 1e300);
    double max_float32 = (double)((1LL << 24) - 1) * ldexp(1, 127 - 23);
    if (reflect_type_overflow_float(TYPE_FLOAT32, max_float32))
        testing_t_errorf_v(t, "%g wrongly overflows float32", max_float32);
    double ovf_float32 = max_float32 + ldexp(1, 127 - 52);
    if (!reflect_type_overflow_float(TYPE_FLOAT32, ovf_float32))
        testing_t_errorf_v(t, "%g should overflow float32", ovf_float32);
    if (!reflect_type_overflow_float(TYPE_FLOAT32, -ovf_float32))
        testing_t_errorf_v(t, "%g should overflow float32", -ovf_float32);
    if (reflect_type_overflow_int(TYPE_INT32, 0x7fffffff))
        testing_t_errorf_v(t, "%d wrongly overflows int32", 0x7fffffff);
    if (reflect_type_overflow_int(TYPE_INT32, -(1LL << 31)))
        testing_t_errorf_v(t, "%d wrongly overflows int32", -(1LL << 31));
    if (!reflect_type_overflow_int(TYPE_INT32, 1LL << 31))
        testing_t_errorf_v(t, "%d should overflow int32", 1LL << 31);
    if (reflect_type_overflow_uint(TYPE_UINT32, 0xffffffffu))
        testing_t_errorf_v(t, "%d wrongly overflows uint32", 0xffffffffu);
    if (!reflect_type_overflow_uint(TYPE_UINT32, 1ULL << 32))
        testing_t_errorf_v(t, "%d should overflow uint32", 1ULL << 32);
}

static void bytes_of_value(void *env) {
    (void)reflect_value_bytes(*(const ReflectValue *)env);
}

static void TestBytes(TestingT *t) {
    char buf[128];
    Int zero = 0;
    ReflectValue v = val(TYPE_INT, &zero);
    if (!panics(bytes_of_value, &v, buf, sizeof buf) ||
        strstr(buf, "on int Value") == NULL)
        testing_t_errorf_v(t, "Bytes of an int: panic %q", cs(buf));
    Slice strs = slice_nil(TYPE_STRING);
    v = val(&burrow_type_Strs, &strs);
    if (!panics(bytes_of_value, &v, buf, sizeof buf) ||
        strstr(buf, "of non-byte slice") == NULL)
        testing_t_errorf_v(t, "Bytes of []string: panic %q", cs(buf));

    uint8_t xs[] = {1, 2, 3, 4};
    Slice x = slice_from(xs, 4, 4, &type_MyByte);
    Slice y = reflect_value_bytes(val(&type_MyBytes, &x));
    if (y.len != 4 || memcmp(y.p, xs, 4) != 0)
        testing_t_fatalf_v(t, "ValueOf(S).Bytes() is not the slice");
    if (y.p != xs)
        testing_t_errorf_v(t, "ValueOf(S).Bytes() points elsewhere");

    Byte8 a = {{1, 2, 3, 4}};
    v = val(&type_A4, &a);
    if (!panics(bytes_of_value, &v, buf, sizeof buf) ||
        strstr(buf, "unaddressable") == NULL)
        testing_t_errorf_v(t, "Bytes of an unaddressable array: panic %q", cs(buf));
    Byte8 *pa = &a;
    v = val(&type_ptr_A4, &pa);
    if (!panics(bytes_of_value, &v, buf, sizeof buf) ||
        strstr(buf, "on ptr Value") == NULL)
        testing_t_errorf_v(t, "Bytes of a pointer: panic %q", cs(buf));
    Slice b = reflect_value_bytes(reflect_value_elem(val(&type_ptr_A4, &pa)));
    if (b.len != 4 || memcmp(b.p, a.v, 4) != 0)
        testing_t_fatalf_v(t, "ValueOf(&a).Elem().Bytes() is not the array");
    if (b.p != a.v)
        testing_t_errorf_v(t, "ValueOf(&a).Elem().Bytes() points elsewhere");

    /* Go's issue 24746: a slice of a named byte type works too. */
    Slice mb = slice_from(xs, 4, 4, &type_MyByte);
    (void)reflect_value_bytes(val(&type_MyBytes_unnamed, &mb));
}

static void TestIndex(TestingT *t) {
    uint8_t xs[] = {1, 2, 3, 4, 5, 6, 7, 8};
    Slice s = slice_from(xs, 8, 8, TYPE_UINT8);
    Any x = reflect_value_interface(reflect_value_index(val(&burrow_type_U8s, &s), 3));
    if (x.t != TYPE_UINT8 || *(const uint8_t *)x.data != xs[3])
        testing_t_errorf_v(t, "xs.Index(3) is not %d", xs[3]);
    Byte8 xa = {{10, 20, 30, 40, 50, 60, 70, 80}};
    x = reflect_value_interface(reflect_value_index(val(&burrow_type_Byte8, &xa), 2));
    if (x.t != TYPE_UINT8 || *(const uint8_t *)x.data != xa.v[2])
        testing_t_errorf_v(t, "xa.Index(2) is not %d", xa.v[2]);
    Str str = BURROW_S("0123456789");
    x = reflect_value_interface(reflect_value_index(val(TYPE_STRING, &str), 3));
    if (x.t != TYPE_UINT8 || *(const uint8_t *)x.data != '3')
        testing_t_errorf_v(t, "s.Index(3) is not %d", '3');
}

static void TestValueString(TestingT *t) {
    Int impl = 0;
    Str s = reflect_value_string(val(&type_Impl, &impl));
    if (!str_eq(s, BURROW_S("<reflect_test.Impl Value>")))
        testing_t_errorf_v(t, "ValueOf(Impl{}).String() = %q, want %q", s,
                           "<reflect_test.Impl Value>");
}

static void TestInvalid(TestingT *t) {
    WithAny w = {{NULL, NULL}};
    ReflectValue v = reflect_value_field(val(&burrow_type_WithAny, &w), 0);
    if (!reflect_value_is_valid(v) || reflect_value_kind(v) != KIND_INTERFACE)
        testing_t_errorf_v(t, "field: IsValid=%t, Kind=%q, want true, Interface",
                           reflect_value_is_valid(v),
                           reflect_kind_string(reflect_value_kind(v)));
    v = reflect_value_elem(v);
    if (reflect_value_is_valid(v) || reflect_value_kind(v) != KIND_INVALID)
        testing_t_errorf_v(t, "field elem: IsValid=%t, Kind=%q, want false, Invalid",
                           reflect_value_is_valid(v),
                           reflect_kind_string(reflect_value_kind(v)));
}

static void len_of(void *env) {
    (void)reflect_value_len(*(const ReflectValue *)env);
}

static void cap_of(void *env) {
    (void)reflect_value_cap(*(const ReflectValue *)env);
}

static void TestValue_Len(TestingT *t) {
    Int3 arr = {{1, 2, 3}};
    Int3 *a = &arr;
    if (reflect_value_len(val(&burrow_type_Int3Ptr, &a)) != 3)
        testing_t_errorf_v(t, "Len = %d want 3",
                           reflect_value_len(val(&burrow_type_Int3Ptr, &a)));
    a = NULL;
    if (reflect_value_len(val(&burrow_type_Int3Ptr, &a)) != 3)
        testing_t_errorf_v(t, "Len = %d want 3",
                           reflect_value_len(val(&burrow_type_Int3Ptr, &a)));
    Int *ptr = NULL;
    ReflectValue v = val(&burrow_type_IntPtr, &ptr);
    char buf[128];
    (void)panics(len_of, &v, buf, sizeof buf);
    if (strcmp(buf, "reflect: call of reflect.Value.Len on ptr to non-array Value") !=
        0)
        testing_t_errorf_v(
            t, "error is %q, want %q", cs(buf),
            "reflect: call of reflect.Value.Len on ptr to non-array Value");
}

static void TestValue_Cap(TestingT *t) {
    Int3 arr = {{1, 2, 3}};
    Int3 *a = &arr;
    if (reflect_value_cap(val(&burrow_type_Int3Ptr, &a)) != 3)
        testing_t_errorf_v(t, "Cap = %d want 3",
                           reflect_value_cap(val(&burrow_type_Int3Ptr, &a)));
    a = NULL;
    if (reflect_value_cap(val(&burrow_type_Int3Ptr, &a)) != 3)
        testing_t_errorf_v(t, "Cap = %d want 3",
                           reflect_value_cap(val(&burrow_type_Int3Ptr, &a)));
    Int *ptr = NULL;
    ReflectValue v = val(&burrow_type_IntPtr, &ptr);
    char buf[128];
    (void)panics(cap_of, &v, buf, sizeof buf);
    if (strcmp(buf, "reflect: call of reflect.Value.Cap on ptr to non-array Value") !=
        0)
        testing_t_errorf_v(
            t, "error is %q, want %q", cs(buf),
            "reflect: call of reflect.Value.Cap on ptr to non-array Value");
}

static void nop(void *env) {
    (void)env;
}

static void TestValuePointerAndUnsafePointer(TestingT *t) {
    Int x = 0;
    Int *ptr = &x;
    Chan *ch = chan_make(heap_allocator(), TYPE_INT, 0);
    Map *m = map_make(heap_allocator(), TYPE_INT, TYPE_INT, 0);
    void *unsafe_ptr = ptr;
    Int one[1] = {0};
    Slice slice = ints_of(one, 1);
    Func fn = {nop, NULL};
    Str s = BURROW_S("foo");
    const struct {
        const char *name;
        ReflectValue v;
        void *want;
    } tests[] = {
        {"pointer", val(&burrow_type_IntPtr, &ptr), ptr},
        {"channel", val(&type_chan_int, &ch), ch},
        {"map", val(&burrow_type_IntIntMap, &m), m},
        {"unsafe.Pointer", val(TYPE_UNSAFE_POINTER, &unsafe_ptr), unsafe_ptr},
        {"function", val(&type_func, &fn), (void *)(uintptr_t)fn.f},
        {"slice", val(&burrow_type_Ints, &slice), one},
        {"string", val(TYPE_STRING, &s), (void *)(uintptr_t)s.p},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        if (reflect_value_pointer(tests[i].v) != (Uintptr)tests[i].want)
            testing_t_errorf_v(t, "%s: unexpected uintptr result", tests[i].name);
        if (reflect_value_unsafe_pointer(tests[i].v) != tests[i].want)
            testing_t_errorf_v(t, "%s: unexpected unsafe.Pointer result",
                               tests[i].name);
    }
    chan_free(ch);
    map_free(m);
}

/* ---------------------------------------------------------- IsNil/IsZero */

static void TestIsNil(TestingT *t) {
    Int x = 0;
    Int *pi = NULL;
    if (!reflect_value_is_nil(val(&burrow_type_IntPtr, &pi)))
        testing_t_errorf_v(t, "*int: IsNil false for nil");
    pi = &x;
    if (reflect_value_is_nil(val(&burrow_type_IntPtr, &pi)))
        testing_t_errorf_v(t, "*int: IsNil true for non-nil");

    Slice si = slice_nil(TYPE_INT);
    if (!reflect_value_is_nil(val(&burrow_type_Ints, &si)))
        testing_t_errorf_v(t, "[]int: IsNil false for nil");
    Int ten[10] = {0};
    si = ints_of(ten, 10);
    if (reflect_value_is_nil(val(&burrow_type_Ints, &si)))
        testing_t_errorf_v(t, "[]int: IsNil true for non-nil");

    Chan *ci = NULL;
    if (!reflect_value_is_nil(val(&type_chan_int, &ci)))
        testing_t_errorf_v(t, "chan int: IsNil false for nil");
    ci = chan_make(heap_allocator(), TYPE_INT, 0);
    if (reflect_value_is_nil(val(&type_chan_int, &ci)))
        testing_t_errorf_v(t, "chan int: IsNil true for non-nil");
    chan_free(ci);

    Map *mi = NULL;
    if (!reflect_value_is_nil(val(&burrow_type_IntIntMap, &mi)))
        testing_t_errorf_v(t, "map[int]int: IsNil false for nil");
    mi = map_make(heap_allocator(), TYPE_INT, TYPE_INT, 0);
    if (reflect_value_is_nil(val(&burrow_type_IntIntMap, &mi)))
        testing_t_errorf_v(t, "map[int]int: IsNil true for non-nil");
    map_free(mi);

    WithAny ii = {{NULL, NULL}};
    ReflectValue f = reflect_value_field(val(&burrow_type_WithAny, &ii), 0);
    if (!reflect_value_is_nil(f))
        testing_t_errorf_v(t, "any: IsNil false for nil");
    Int two = 2;
    ii.v = BURROW_ANY(TYPE_INT, &two);
    f = reflect_value_field(val(&burrow_type_WithAny, &ii), 0);
    if (reflect_value_is_nil(f))
        testing_t_errorf_v(t, "any: IsNil true for non-nil");

    Func fi = {NULL, NULL};
    if (!reflect_value_is_nil(val(&type_func, &fi)))
        testing_t_errorf_v(t, "func: IsNil false for nil");
    fi.f = nop;
    if (reflect_value_is_nil(val(&type_func, &fi)))
        testing_t_errorf_v(t, "func: IsNil true for non-nil");
}

static void TestIsZero(TestingT *t) {
    bool yes = true, no = false;
    Int i0 = 0, i1 = 1;
    int8_t i80 = 0, i81 = 1;
    uint64_t u0 = 0, u1 = 1;
    float f0 = 0, f1 = 1.2f;
    double d0 = 0, d1 = 1.2, dneg = -0.0;
    Complex128 c0 = {0, 0}, c1 = {1.2, 0}, cneg = {-0.0, -0.0};
    Uintptr p0 = 0, p128 = 128;
    Str5 s5 = {{BURROW_STR_EMPTY}};
    Str5 s5a = {{BURROW_STR_EMPTY, BURROW_STR_EMPTY, BURROW_STR_EMPTY, BURROW_S("a")}};
    Ints3 is3 = {{{0}}};
    Int one = 1;
    Ints3 is3b = {{ints_of(&one, 1)}};
    Chan *cnil = NULL;
    Func fnil = {NULL, NULL}, fsome = {nop, NULL};
    Map *mnil = NULL;
    Int *pnil = NULL, *pnew = &i0;
    Slice snil = slice_nil(TYPE_STRING);
    Str empty[1];
    Slice sempty = slice_from(empty, 0, 0, TYPE_STRING);
    Str str0 = BURROW_STR_EMPTY, str1 = BURROW_S("abc");
    Basic b0 = {0, 0}, b1 = {1, 0};
    const struct {
        const Type *t;
        const void *p;
        bool want;
    } tests[] = {
        {TYPE_BOOL, &yes, false},
        {TYPE_BOOL, &no, true},
        {TYPE_INT, &i0, true},
        {TYPE_INT, &i1, false},
        {TYPE_INT8, &i80, true},
        {TYPE_INT8, &i81, false},
        {TYPE_UINT64, &u0, true},
        {TYPE_UINT64, &u1, false},
        {TYPE_FLOAT32, &f0, true},
        {TYPE_FLOAT32, &f1, false},
        {TYPE_FLOAT64, &d0, true},
        {TYPE_FLOAT64, &d1, false},
        {TYPE_FLOAT64, &dneg, true},
        {TYPE_COMPLEX128, &c0, true},
        {TYPE_COMPLEX128, &c1, false},
        {TYPE_COMPLEX128, &cneg, true},
        {TYPE_UINTPTR, &p0, true},
        {TYPE_UINTPTR, &p128, false},
        {&burrow_type_Str5, &s5, true},
        {&burrow_type_Str5, &s5a, false},
        {&burrow_type_Ints3, &is3, true},
        {&burrow_type_Ints3, &is3b, false},
        {&type_chan_int, &cnil, true},
        {&type_func, &fnil, true},
        {&type_func, &fsome, false},
        {&burrow_type_IntIntMap, &mnil, true},
        {&burrow_type_IntPtr, &pnil, true},
        {&burrow_type_IntPtr, &pnew, false},
        {&burrow_type_Strs, &sempty, false},
        {&burrow_type_Strs, &snil, true},
        {TYPE_STRING, &str0, true},
        {TYPE_STRING, &str1, false},
        {&burrow_type_Basic, &b0, true},
        {&burrow_type_Basic, &b1, false},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        bool got = reflect_value_is_zero(val(tests[i].t, tests[i].p));
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%d: IsZero((%q)) = %t, want %t", i,
                               reflect_type_string(tests[i].t), got, tests[i].want);
    }
}

/* ------------------------------------------------------------- maps */

/* Go's iterateToString for map[string]int and map[int]string: each pair as
 * "k: v", sorted, joined. */
static int cmp_cstr(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void value_text(ReflectValue v, char *buf, size_t n) {
    if (reflect_value_kind(v) == KIND_STRING) {
        Str s = reflect_value_string(v);
        snprintf(buf, n, "%.*s", (int)s.len, (const char *)s.p);
    } else {
        snprintf(buf, n, "%lld", (long long)reflect_value_int(v));
    }
}

static void iterate_to_string(ReflectMapIter *it, char *out, size_t n) {
    char lines[8][40];
    int count = 0;
    while (reflect_map_iter_next(it) && count < 8) {
        char k[16], v[16];
        value_text(reflect_map_iter_key(it), k, sizeof k);
        value_text(reflect_map_iter_value(it), v, sizeof v);
        snprintf(lines[count++], sizeof lines[0], "%s: %s", k, v);
    }
    qsort(lines, (size_t)count, sizeof lines[0], cmp_cstr);
    size_t at = 0;
    at += (size_t)snprintf(out + at, n - at, "[");
    for (int i = 0; i < count; i++)
        at += (size_t)snprintf(out + at, n - at, "%s%s", i > 0 ? ", " : "", lines[i]);
    snprintf(out + at, n - at, "]");
}

static Map *str_int_map(void) {
    Map *m = map_make(heap_allocator(), TYPE_STRING, TYPE_INT, 0);
    static const char *const keys[] = {"one", "two", "three"};
    for (Int i = 0; i < 3; i++) {
        Str k = cs(keys[i]);
        Int v = i + 1;
        map_set(m, &k, &v);
    }
    return m;
}

static void TestMapIterNonEmptyMap(TestingT *t) {
    Map *m = str_int_map();
    Arena ar;
    arena_init(&ar, heap_allocator(), 256);
    ReflectMapIter *it =
        reflect_value_map_range(val(&burrow_type_StrIntMap, &m), arena_allocator(&ar));
    char got[128];
    iterate_to_string(it, got, sizeof got);
    if (strcmp(got, "[one: 1, three: 3, two: 2]") != 0)
        testing_t_errorf_v(t, "iterator returned %s (after sorting), want %s", got,
                           "[one: 1, three: 3, two: 2]");
    arena_free(&ar);
    map_free(m);
}

static void TestMapIterNilMap(TestingT *t) {
    Map *m = NULL;
    Arena ar;
    arena_init(&ar, heap_allocator(), 256);
    ReflectMapIter *it =
        reflect_value_map_range(val(&burrow_type_StrIntMap, &m), arena_allocator(&ar));
    char got[128];
    iterate_to_string(it, got, sizeof got);
    if (strcmp(got, "[]") != 0)
        testing_t_errorf_v(t, "non-empty result iteratoring nil map: %s", got);
    arena_free(&ar);
}

static void iter_next(void *env) {
    (void)reflect_map_iter_next(env);
}

static void TestMapIterReset(TestingT *t) {
    ReflectMapIter it;
    memset(&it, 0, sizeof it);
    char buf[128];
    if (!panics(iter_next, &it, buf, sizeof buf))
        testing_t_errorf_v(t, "Next did not panic");

    Map *m = str_int_map();
    reflect_map_iter_reset(&it, val(&burrow_type_StrIntMap, &m));
    char got[128];
    iterate_to_string(&it, got, sizeof got);
    if (strcmp(got, "[one: 1, three: 3, two: 2]") != 0)
        testing_t_errorf_v(t, "iterator returned %s (after sorting), want %s", got,
                           "[one: 1, three: 3, two: 2]");

    reflect_map_iter_reset(&it, (ReflectValue){0});
    if (!panics(iter_next, &it, buf, sizeof buf))
        testing_t_errorf_v(t, "Next did not panic");

    Map *m2 = map_make(heap_allocator(), TYPE_INT, TYPE_STRING, 0);
    static const char *const names[] = {"one", "two", "three"};
    for (Int i = 0; i < 3; i++) {
        Int k = i + 1;
        Str v = cs(names[i]);
        map_set(m2, &k, &v);
    }
    reflect_map_iter_reset(&it, val(&burrow_type_IntStrMap, &m2));
    iterate_to_string(&it, got, sizeof got);
    if (strcmp(got, "[1: one, 2: two, 3: three]") != 0)
        testing_t_errorf_v(t, "iterator returned %s (after sorting), want %s", got,
                           "[1: one, 2: two, 3: three]");
    map_free(m);
    map_free(m2);
}

static void TestMapIterNext(TestingT *t) {
    /* The first call to Next should reflect any insertions to the map since
     * the iterator was created. */
    Map *m = map_make(heap_allocator(), TYPE_STRING, TYPE_INT, 0);
    ReflectMapIter it;
    reflect_map_iter_reset(&it, val(&burrow_type_StrIntMap, &m));
    Str k = BURROW_S("one");
    Int v = 1;
    map_set(m, &k, &v);
    char got[128];
    iterate_to_string(&it, got, sizeof got);
    if (strcmp(got, "[one: 1]") != 0)
        testing_t_errorf_v(t, "iterator returned deleted elements: got %s, want %s",
                           got, "[one: 1]");
    map_free(m);
}

/* ----------------------------------------------------------- Seq, Seq2 */

typedef struct SeqState {
    TestingT *t;
    const Type *want_type;
    int64_t want[8];
    Int n;
    Int count;
} SeqState;

static bool seq_check(void *env, const void *p) {
    SeqState *s = env;
    ReflectValue v = *(const ReflectValue *)p;
    if (s->want_type != NULL && v.typ != s->want_type)
        testing_t_errorf_v(s->t, "value %d has type %q", s->count,
                           reflect_type_string(v.typ));
    int64_t got = reflect_value_can_uint(v) ? (int64_t)reflect_value_uint(v)
                                            : reflect_value_int(v);
    if (s->count >= s->n || got != s->want[s->count])
        testing_t_errorf_v(s->t, "value %d: got %d", s->count, got);
    s->count++;
    return true;
}

static void run_seq(TestingT *t, const char *name, ReflectValue v,
                    const Type *want_type, const int64_t *want, Int n) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 256);
    SeqState s = {t, want_type, {0}, n, 0};
    memcpy(s.want, want, (size_t)n * sizeof want[0]);
    IterSeq seq = reflect_value_seq(v, arena_allocator(&ar));
    seq.f(seq.env, BURROW_FN(IterYield, seq_check, &s));
    if (s.count != n)
        testing_t_errorf_v(t, "%s: looped %d times, want %d", name, s.count, n);
    arena_free(&ar);
}

static bool seq_count_key(void *env, const void *p) {
    Map *left = env;
    Str k = reflect_value_string(*(const ReflectValue *)p);
    map_del(left, &k);
    return true;
}

static void TestValueSeq(TestingT *t) {
    static const int64_t four[] = {0, 1, 2, 3};
    Int n4 = 4;
    run_seq(t, "int", val(TYPE_INT, &n4), TYPE_INT, four, 4);
    int8_t i8 = 4;
    run_seq(t, "int8", val(TYPE_INT8, &i8), TYPE_INT8, four, 4);
    uint64_t u64 = 4;
    run_seq(t, "uint", val(TYPE_UINT64, &u64), TYPE_UINT64, four, 4);
    uint8_t u8 = 4;
    run_seq(t, "uint8", val(TYPE_UINT8, &u8), TYPE_UINT8, four, 4);
    Int4 a4 = {{1, 2, 3, 4}};
    Int4 *pa4 = &a4;
    run_seq(t, "*[4]int", val(&burrow_type_Int4Ptr, &pa4), TYPE_INT, four, 4);
    Int3 *pnil = NULL;
    run_seq(t, "nil *[3]int", val(&burrow_type_Int3Ptr, &pnil), TYPE_INT, four, 3);
    run_seq(t, "[4]int", val(&burrow_type_Int4, &a4), TYPE_INT, four, 4);
    Slice s4 = ints_of(a4.v, 4);
    run_seq(t, "[]int", val(&burrow_type_Ints, &s4), TYPE_INT, four, 4);
    static const int64_t idx[] = {0, 1, 2, 5};
    Str str = BURROW_S("12\xe8\xaf\xad\xe8\xa8\x80");
    run_seq(t, "string", val(TYPE_STRING, &str), TYPE_INT, idx, 4);

    Map *m = map_make(heap_allocator(), TYPE_STRING, TYPE_INT, 0);
    Map *left = map_make(heap_allocator(), TYPE_STRING, TYPE_INT, 0);
    static const char *const keys[] = {"1", "2", "3", "4"};
    for (Int i = 0; i < 4; i++) {
        Str k = cs(keys[i]);
        Int v = i + 1;
        map_set(m, &k, &v);
        map_set(left, &k, &v);
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 256);
    IterSeq seq =
        reflect_value_seq(val(&burrow_type_StrIntMap, &m), arena_allocator(&ar));
    seq.f(seq.env, BURROW_FN(IterYield, seq_count_key, left));
    if (map_len(left) != 0)
        testing_t_errorf_v(t, "map[string]int: should loop four times");
    map_free(m);
    map_free(left);

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 3);
    for (Int i = 0; i < 3; i++)
        chan_send(c, &i);
    chan_close(c);
    static const int64_t three[] = {0, 1, 2};
    run_seq(t, "chan int", val(&type_chan_int, &c), TYPE_INT, three, 3);
    chan_free(c);
    arena_free(&ar);
}

typedef struct Seq2State {
    TestingT *t;
    int64_t want_k[8];
    int64_t want_v[8];
    Int n;
    Int count;
} Seq2State;

static bool seq2_check(void *env, const void *pk, const void *pv) {
    Seq2State *s = env;
    int64_t k = reflect_value_int(*(const ReflectValue *)pk);
    int64_t v = reflect_value_int(*(const ReflectValue *)pv);
    if (s->count >= s->n || k != s->want_k[s->count] || v != s->want_v[s->count])
        testing_t_errorf_v(s->t, "pair %d: got %d, %d", s->count, k, v);
    s->count++;
    return true;
}

static void run_seq2(TestingT *t, const char *name, ReflectValue v, const int64_t *k,
                     const int64_t *want, Int n) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 256);
    Seq2State s = {t, {0}, {0}, n, 0};
    memcpy(s.want_k, k, (size_t)n * sizeof k[0]);
    memcpy(s.want_v, want, (size_t)n * sizeof want[0]);
    IterSeq2 seq = reflect_value_seq2(v, arena_allocator(&ar));
    seq.f(seq.env, BURROW_FN(IterYield2, seq2_check, &s));
    if (s.count != n)
        testing_t_errorf_v(t, "%s: looped %d times, want %d", name, s.count, n);
    arena_free(&ar);
}

static void TestValueSeq2(TestingT *t) {
    static const int64_t idx[] = {0, 1, 2, 3};
    static const int64_t vals[] = {1, 2, 3, 4};
    Int4 a4 = {{1, 2, 3, 4}};
    Int4 *pa4 = &a4;
    run_seq2(t, "*[4]int", val(&burrow_type_Int4Ptr, &pa4), idx, vals, 4);
    run_seq2(t, "[4]int", val(&burrow_type_Int4, &a4), idx, vals, 4);
    Slice s4 = ints_of(a4.v, 4);
    run_seq2(t, "[]int", val(&burrow_type_Ints, &s4), idx, vals, 4);
    static const int64_t sidx[] = {0, 1, 2, 5};
    static const int64_t runes[] = {'1', '2', 0x8bed, 0x8a00};
    Str str = BURROW_S("12\xe8\xaf\xad\xe8\xa8\x80");
    run_seq2(t, "string", val(TYPE_STRING, &str), sidx, runes, 4);
}

/* --------------------------------------------------- Equal, Comparable */

static void TestValue_Equal(TestingT *t) {
    bool yes = true;
    Int one = 1, two = 2;
    int8_t i8 = 1;
    uint64_t u64 = 1;
    float f32 = 1;
    double f64 = 1;
    Complex128 c = {1, 1};
    Chan *cnil = NULL;
    Int *pi = &one;
    Pair p1 = {1, 0}, p1b = {1, 0}, p2 = {2, 0};
    Func fn = {nop, NULL};
    WithAny nil_iface = {{NULL, NULL}};
    ReflectValue inil = reflect_value_field(val(&burrow_type_WithAny, &nil_iface), 0);
    const struct {
        ReflectValue v, u;
        bool eq;
    } tests[] = {
        {{0}, {0}, true},
        {val(TYPE_BOOL, &yes), val(TYPE_BOOL, &yes), true},
        {val(TYPE_INT, &one), val(TYPE_INT, &one), true},
        {val(TYPE_INT8, &i8), val(TYPE_INT8, &i8), true},
        {val(TYPE_UINT64, &u64), val(TYPE_UINT64, &u64), true},
        {val(TYPE_FLOAT32, &f32), val(TYPE_FLOAT32, &f32), true},
        {val(TYPE_FLOAT64, &f64), val(TYPE_FLOAT64, &f64), true},
        {val(TYPE_COMPLEX128, &c), val(TYPE_COMPLEX128, &c), true},
        {val(&type_func, &fn), {0}, false},
        {reflect_value_elem(val(&burrow_type_IntPtr, &pi)), val(TYPE_INT, &one), true},
        {val(&type_chan_int, &cnil), {0}, false},
        {val(&type_chan_int, &cnil), val(&type_chan_int, &cnil), true},
        {val(&burrow_type_IntPtr, &pi), val(&burrow_type_IntPtr, &pi), true},
        {val(&burrow_type_Pair, &p1), val(&burrow_type_Pair, &p1b), true},
        {val(&burrow_type_Pair, &p1), val(&burrow_type_Pair, &p2), false},
        {inil, inil, true},
        {val(TYPE_INT, &one), val(TYPE_INT, &two), false},
        {val(TYPE_INT, &one), val(TYPE_INT8, &i8), false},
    };
    for (Int i = 0; i < NELEM(tests); i++)
        if (reflect_value_equal(tests[i].v, tests[i].u) != tests[i].eq)
            testing_t_errorf_v(t, "test #%d: Equal is %t, want %t", i, !tests[i].eq,
                               tests[i].eq);
}

static void equal_values(void *env) {
    const ReflectValue *v = env;
    (void)reflect_value_equal(v[0], v[1]);
}

static void TestValue_EqualNonComparable(TestingT *t) {
    Int one = 1;
    Slice s = ints_of(&one, 1);
    Map *m = NULL;
    Func fn = {nop, NULL};
    ReflectValue values[][2] = {
        {val(&burrow_type_Ints, &s), val(&burrow_type_Ints, &s)},
        {val(&burrow_type_IntIntMap, &m), val(&burrow_type_IntIntMap, &m)},
        {val(&type_func, &fn), val(&type_func, &fn)},
    };
    for (Int i = 0; i < NELEM(values); i++) {
        char buf[128];
        if (!panics(equal_values, values[i], buf, sizeof buf))
            testing_t_errorf_v(t, "%q == %q didn't panic",
                               reflect_type_string(values[i][0].typ),
                               reflect_type_string(values[i][1].typ));
    }
}

static void TestValue_Comparable(TestingT *t) {
    Int one = 1;
    Slice s = ints_of(&one, 1);
    Ints3 arr = {{s, s, s}};
    Str5 strs = {{BURROW_STR_EMPTY}};
    Pair p = {1, 2};
    WithAny holds_int = {BURROW_ANY(TYPE_INT, &one)};
    WithAny holds_slice = {BURROW_ANY(&burrow_type_Ints, &s)};
    WithAny holds_nil = {{NULL, NULL}};
    Func fn = {nop, NULL};
    const struct {
        ReflectValue v;
        bool comparable;
    } tests[] = {
        {{0}, false},
        {val(TYPE_INT, &one), true},
        {val(&burrow_type_Ints, &s), false},
        {val(&burrow_type_Ints3, &arr), false},
        {val(&burrow_type_Str5, &strs), true},
        {val(&burrow_type_Pair, &p), true},
        {val(&type_func, &fn), false},
        {reflect_value_field(val(&burrow_type_WithAny, &holds_int), 0), true},
        {reflect_value_field(val(&burrow_type_WithAny, &holds_slice), 0), false},
        {reflect_value_field(val(&burrow_type_WithAny, &holds_nil), 0), true},
        {val(&burrow_type_WithAny, &holds_int), true},
        {val(&burrow_type_WithAny, &holds_slice), false},
    };
    for (Int i = 0; i < NELEM(tests); i++)
        if (reflect_value_comparable(tests[i].v) != tests[i].comparable)
            testing_t_errorf_v(t, "test #%d: Comparable is %t, want %t", i,
                               !tests[i].comparable, tests[i].comparable);
}

/* -------------------------------------------------------------- Swapper */

static void TestSwapper(TestingT *t) {
    Int ints[] = {1, 20, 300};
    Slice si = ints_of(ints, 3);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&burrow_type_Ints, &si)), 0, 2);
    if (ints[0] != 300 || ints[1] != 20 || ints[2] != 1)
        testing_t_errorf_v(t, "[]int: %d %d %d", ints[0], ints[1], ints[2]);

    Uintptr ups[] = {1, 20, 300};
    Slice su = slice_from(ups, 3, 3, TYPE_UINTPTR);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&burrow_type_Uintptrs, &su)), 0, 2);
    if (ups[0] != 300 || ups[2] != 1)
        testing_t_errorf_v(t, "[]uintptr: %d %d", (Int)ups[0], (Int)ups[2]);

    int16_t i16[] = {1, 20, 300};
    Slice s16 = slice_from(i16, 3, 3, TYPE_INT16);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&burrow_type_Int16s, &s16)), 0, 2);
    if (i16[0] != 300 || i16[2] != 1)
        testing_t_errorf_v(t, "[]int16: %d %d", i16[0], i16[2]);

    int8_t i8[] = {1, 20, 100};
    Slice s8 = slice_from(i8, 3, 3, TYPE_INT8);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&burrow_type_Int8s, &s8)), 0, 2);
    if (i8[0] != 100 || i8[2] != 1)
        testing_t_errorf_v(t, "[]int8: %d %d", i8[0], i8[2]);

    Int a = 0, b = 0, c = 0;
    Int *ptrs[] = {&a, &b, &c};
    Slice sp = slice_from(ptrs, 3, 3, &type_ptr_I);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&type_ptrs_I, &sp)), 0, 2);
    if (ptrs[0] != &c || ptrs[1] != &b || ptrs[2] != &a)
        testing_t_errorf_v(t, "[]*I: not swapped");

    Str names[] = {BURROW_S("eric"), BURROW_S("sergey"), BURROW_S("larry")};
    Slice ss = slice_from(names, 3, 3, TYPE_STRING);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&burrow_type_Strs, &ss)), 0, 2);
    if (!str_eq(names[0], BURROW_S("larry")) || !str_eq(names[2], BURROW_S("eric")))
        testing_t_errorf_v(t, "[]string: %q %q", names[0], names[2]);

    Str snames[] = {BURROW_S("eric"), BURROW_S("sergey"), BURROW_S("larry")};
    Slice sS = slice_from(snames, 3, 3, &type_S);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&type_Ss, &sS)), 0, 2);
    if (!str_eq(snames[0], BURROW_S("larry")) || !str_eq(snames[2], BURROW_S("eric")))
        testing_t_errorf_v(t, "[]S: %q %q", snames[0], snames[2]);

    Pair pairs[] = {{1, 2}, {3, 4}, {5, 6}};
    Slice spair = slice_from(pairs, 3, 3, &burrow_type_Pair);
    BURROW_CALLF(reflect_swapper(BURROW_ANY(&burrow_type_Pairs, &spair)), 0, 2);
    if (pairs[0].x != 5 || pairs[0].y != 6 || pairs[2].x != 1 || pairs[2].y != 2)
        testing_t_errorf_v(t, "[]pair: not swapped");
}

/* ------------------------------------------------------------ DeepEqual */

static void TestDeepEqual(TestingT *t) {
    Int one = 1, one_b = 1, two = 2;
    int32_t i32a = 1, i32b = 1, i32c = 2;
    int64_t i64 = 1;
    double half = 0.5, half_b = 0.5, six = 0.6, onef = 1.0;
    Str hello = BURROW_S("hello"), hello_b = BURROW_S("hello"), hey = BURROW_S("hey");
    Int z10a[10] = {0}, z10b[10] = {0}, z11[11] = {0};
    Slice s10a = ints_of(z10a, 10), s10b = ints_of(z10b, 10), s11 = ints_of(z11, 11);
    Int3 a123 = {{1, 2, 3}}, a123b = {{1, 2, 3}}, a124 = {{1, 2, 4}};
    Int3 *pa123 = &a123, *pa123b = &a123b, *pa124 = &a124;
    Basic b1 = {1, 0.5f}, b1b = {1, 0.5f}, b2 = {1, 0.6f};
    NotBasic nb = {1, 0.5f};
    uint8_t by[] = {1, 2, 3}, by_b[] = {1, 2, 3};
    Slice sby = slice_from(by, 3, 3, TYPE_UINT8),
          sby_b = slice_from(by_b, 3, 3, TYPE_UINT8);
    Slice smb = slice_from(by, 3, 3, &type_MyByte),
          smb_b = slice_from(by_b, 3, 3, &type_MyByte);
    Func fn1 = {NULL, NULL}, fn2 = {NULL, NULL}, fn3 = {nop, NULL};
    double nan = (double)NAN;
    Float1 fnan = {{(double)NAN}}, fnan_b = {{(double)NAN}};
    Float1 *pfnan = &fnan, *pfnan_b = &fnan_b;
    Slice snan = slice_from(&nan, 1, 1, TYPE_FLOAT64);
    Int none[1];
    Slice sempty = ints_of(none, 0), sempty_b = ints_of(none, 0),
          snil = slice_nil(TYPE_INT);
    Uint u1 = 1;

    Map *m12 = map_make(heap_allocator(), TYPE_INT, TYPE_STRING, 0);
    Map *m21 = map_make(heap_allocator(), TYPE_INT, TYPE_STRING, 0);
    Map *m13 = map_make(heap_allocator(), TYPE_INT, TYPE_STRING, 0);
    Map *m1 = map_make(heap_allocator(), TYPE_INT, TYPE_STRING, 0);
    Map *mu = map_make(heap_allocator(), TYPE_UINT, TYPE_STRING, 0);
    Map *mempty = map_make(heap_allocator(), TYPE_INT, TYPE_INT, 0);
    Map *mempty_b = map_make(heap_allocator(), TYPE_INT, TYPE_INT, 0);
    Map *mnil = NULL;
    Int k1 = 1, k2 = 2, k3 = 3;
    Str sone = BURROW_S("one"), stwo = BURROW_S("two");
    map_set(m12, &k1, &sone);
    map_set(m12, &k2, &stwo);
    map_set(m21, &k2, &stwo);
    map_set(m21, &k1, &sone);
    map_set(m13, &k1, &sone);
    map_set(m13, &k3, &stwo);
    map_set(m1, &k1, &sone);
    map_set(mu, &u1, &sone);
    Uint u2 = 2;
    map_set(mu, &u2, &stwo);

    SelfPtr sa_inner = {NULL, BURROW_S("a")}, sb_inner = {NULL, BURROW_S("b")};
    SelfPtr sa = {&sa_inner, BURROW_STR_EMPTY}, sb = {&sb_inner, BURROW_STR_EMPTY};
    SelfPtr *psa = &sa, *psb = &sb;

    void *loop1, *loop2;
    loop1 = &loop2;
    loop2 = &loop1;
    void **ploop1 = &loop1, **ploop2 = &loop2;

    const struct {
        Any a, b;
        bool eq;
    } tests[] = {
        /* Equalities */
        {{NULL, NULL}, {NULL, NULL}, true},
        {BURROW_ANY(TYPE_INT, &one), BURROW_ANY(TYPE_INT, &one_b), true},
        {BURROW_ANY(TYPE_INT32, &i32a), BURROW_ANY(TYPE_INT32, &i32b), true},
        {BURROW_ANY(TYPE_FLOAT64, &half), BURROW_ANY(TYPE_FLOAT64, &half_b), true},
        {BURROW_ANY(TYPE_STRING, &hello), BURROW_ANY(TYPE_STRING, &hello_b), true},
        {BURROW_ANY(&burrow_type_Ints, &s10a), BURROW_ANY(&burrow_type_Ints, &s10b),
         true},
        {BURROW_ANY(&burrow_type_Int3Ptr, &pa123),
         BURROW_ANY(&burrow_type_Int3Ptr, &pa123b), true},
        {BURROW_ANY(&burrow_type_Basic, &b1), BURROW_ANY(&burrow_type_Basic, &b1b),
         true},
        {BURROW_ANY(&burrow_type_IntStrMap, &m12),
         BURROW_ANY(&burrow_type_IntStrMap, &m21), true},
        {BURROW_ANY(&type_func, &fn1), BURROW_ANY(&type_func, &fn2), true},
        {BURROW_ANY(&burrow_type_U8s, &sby), BURROW_ANY(&burrow_type_U8s, &sby_b),
         true},
        {BURROW_ANY(&type_MyBytes_unnamed, &smb),
         BURROW_ANY(&type_MyBytes_unnamed, &smb_b), true},
        {BURROW_ANY(&type_MyBytes, &smb), BURROW_ANY(&type_MyBytes, &smb_b), true},

        /* Inequalities */
        {BURROW_ANY(TYPE_INT, &one), BURROW_ANY(TYPE_INT, &two), false},
        {BURROW_ANY(TYPE_INT32, &i32a), BURROW_ANY(TYPE_INT32, &i32c), false},
        {BURROW_ANY(TYPE_FLOAT64, &half), BURROW_ANY(TYPE_FLOAT64, &six), false},
        {BURROW_ANY(TYPE_STRING, &hello), BURROW_ANY(TYPE_STRING, &hey), false},
        {BURROW_ANY(&burrow_type_Ints, &s10a), BURROW_ANY(&burrow_type_Ints, &s11),
         false},
        {BURROW_ANY(&burrow_type_Int3Ptr, &pa123),
         BURROW_ANY(&burrow_type_Int3Ptr, &pa124), false},
        {BURROW_ANY(&burrow_type_Basic, &b1), BURROW_ANY(&burrow_type_Basic, &b2),
         false},
        {BURROW_ANY(&burrow_type_IntStrMap, &m13),
         BURROW_ANY(&burrow_type_IntStrMap, &m21), false},
        {BURROW_ANY(&burrow_type_IntStrMap, &m1),
         BURROW_ANY(&burrow_type_IntStrMap, &m21), false},
        {BURROW_ANY(&burrow_type_IntStrMap, &m21),
         BURROW_ANY(&burrow_type_IntStrMap, &m1), false},
        {{NULL, NULL}, BURROW_ANY(TYPE_INT, &one), false},
        {BURROW_ANY(TYPE_INT, &one), {NULL, NULL}, false},
        {BURROW_ANY(&type_func, &fn1), BURROW_ANY(&type_func, &fn3), false},
        {BURROW_ANY(&type_func, &fn3), BURROW_ANY(&type_func, &fn3), false},
        {BURROW_ANY(&burrow_type_SelfPtrPtr, &psa),
         BURROW_ANY(&burrow_type_SelfPtrPtr, &psb), false},

        /* Fun with floating point. */
        {BURROW_ANY(TYPE_FLOAT64, &nan), BURROW_ANY(TYPE_FLOAT64, &nan), false},
        {BURROW_ANY(&burrow_type_Float1Ptr, &pfnan),
         BURROW_ANY(&burrow_type_Float1Ptr, &pfnan_b), false},
        {BURROW_ANY(&burrow_type_Float1Ptr, &pfnan),
         BURROW_ANY(&burrow_type_Float1Ptr, &pfnan), true},
        {BURROW_ANY(&burrow_type_Float64s, &snan),
         BURROW_ANY(&burrow_type_Float64s, &snan), true},

        /* Nil vs empty: not the same. */
        {BURROW_ANY(&burrow_type_Ints, &sempty), BURROW_ANY(&burrow_type_Ints, &snil),
         false},
        {BURROW_ANY(&burrow_type_Ints, &sempty),
         BURROW_ANY(&burrow_type_Ints, &sempty_b), true},
        {BURROW_ANY(&burrow_type_Ints, &snil), BURROW_ANY(&burrow_type_Ints, &snil),
         true},
        {BURROW_ANY(&burrow_type_IntIntMap, &mempty),
         BURROW_ANY(&burrow_type_IntIntMap, &mnil), false},
        {BURROW_ANY(&burrow_type_IntIntMap, &mempty),
         BURROW_ANY(&burrow_type_IntIntMap, &mempty_b), true},
        {BURROW_ANY(&burrow_type_IntIntMap, &mnil),
         BURROW_ANY(&burrow_type_IntIntMap, &mnil), true},

        /* Mismatched types */
        {BURROW_ANY(TYPE_INT, &one), BURROW_ANY(TYPE_FLOAT64, &onef), false},
        {BURROW_ANY(TYPE_INT32, &i32a), BURROW_ANY(TYPE_INT64, &i64), false},
        {BURROW_ANY(TYPE_FLOAT64, &half), BURROW_ANY(TYPE_STRING, &hello), false},
        {BURROW_ANY(&burrow_type_Ints, &s10a), BURROW_ANY(&burrow_type_Int3, &a123),
         false},
        {BURROW_ANY(&burrow_type_Basic, &b1), BURROW_ANY(&burrow_type_NotBasic, &nb),
         false},
        {BURROW_ANY(&burrow_type_UintStrMap, &mu),
         BURROW_ANY(&burrow_type_IntStrMap, &m21), false},
        {BURROW_ANY(&burrow_type_U8s, &sby), BURROW_ANY(&type_MyBytes_unnamed, &smb),
         false},
        {BURROW_ANY(&type_MyBytes_unnamed, &smb), BURROW_ANY(&type_MyBytes, &smb_b),
         false},
        {BURROW_ANY(&burrow_type_U8s, &sby), BURROW_ANY(&type_MyBytes, &smb), false},

        /* Possible loops. */
        {BURROW_ANY(&type_ptr_Loop, &ploop1), BURROW_ANY(&type_ptr_Loop, &ploop1),
         true},
        {BURROW_ANY(&type_ptr_Loop, &ploop1), BURROW_ANY(&type_ptr_Loop, &ploop2),
         true},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        bool r = reflect_deep_equal(tests[i].a, tests[i].b);
        if (r != tests[i].eq)
            testing_t_errorf_v(t, "#%d: DeepEqual(%q, %q) = %t, want %t", i,
                               reflect_type_string(tests[i].a.t),
                               reflect_type_string(tests[i].b.t), r, tests[i].eq);
    }
    map_free(m12);
    map_free(m21);
    map_free(m13);
    map_free(m1);
    map_free(mu);
    map_free(mempty);
    map_free(mempty_b);
}

static void TestDeepEqualRecursiveStruct(TestingT *t) {
    Recursive a, b;
    a = (Recursive){12, &a};
    b = (Recursive){12, &b};
    RecursivePtr pa = &a, pb = &b;
    if (!reflect_deep_equal(BURROW_ANY(&burrow_type_RecursivePtr, &pa),
                            BURROW_ANY(&burrow_type_RecursivePtr, &pb)))
        testing_t_errorf_v(t, "DeepEqual(recursive same) = false, want true");
}

#define TESTS(X)                                                                       \
    X(TestNegativeKindString)                                                          \
    X(TestFieldByIndex)                                                                \
    X(TestFieldByName)                                                                 \
    X(TestFields)                                                                      \
    X(TestFieldByIndexErr)                                                             \
    X(TestTypeFieldOutOfRangePanic)                                                    \
    X(TestTagGet)                                                                      \
    X(TestCanIntUintFloatComplex)                                                      \
    X(TestValueOverflow)                                                               \
    X(TestTypeOverflow)                                                                \
    X(TestBytes)                                                                       \
    X(TestIndex)                                                                       \
    X(TestValueString)                                                                 \
    X(TestInvalid)                                                                     \
    X(TestValue_Len)                                                                   \
    X(TestValue_Cap)                                                                   \
    X(TestValuePointerAndUnsafePointer)                                                \
    X(TestIsNil)                                                                       \
    X(TestIsZero)                                                                      \
    X(TestMapIterNonEmptyMap)                                                          \
    X(TestMapIterNilMap)                                                               \
    X(TestMapIterReset)                                                                \
    X(TestMapIterNext)                                                                 \
    X(TestValueSeq)                                                                    \
    X(TestValueSeq2)                                                                   \
    X(TestValue_Equal)                                                                 \
    X(TestValue_EqualNonComparable)                                                    \
    X(TestValue_Comparable)                                                            \
    X(TestSwapper)                                                                     \
    X(TestDeepEqual)                                                                   \
    X(TestDeepEqualRecursiveStruct)
TESTING_MAIN(TESTS)
