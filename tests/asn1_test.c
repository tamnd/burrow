/* Derived from Go's src/encoding/asn1/asn1_test.go and marshal_test.go.
 * Go source: go1.27.1.
 *
 * Every test in the package is here, plus TestNoMemory. Go's
 * TestUnmarshalWithNilOrNonPointer has a case for a value that is not a
 * pointer, which an Any cannot be, so that case is left out. The two deep
 * nesting tests run on a goroutine with a big stack, the way the gob ones do.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/chan.h"
#include "burrow/declare.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/hex.h"
#include "burrow/error.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include "../src/encoding/asn1_internal.h"
#include "asn1_test_data.h"

#include <string.h>

/* ------------------------------------------------------------------ helpers */

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, (Int)strlen(s)};
}

static Slice lit(const char *s, Int n) {
    return (Slice){(void *)(uintptr_t)s, n, n, TYPE_BYTE};
}

#define LIT(s) lit(s, (Int)sizeof(s) - 1)
#define BYTES(...)                                                                     \
    ((Slice){(Byte[]){__VA_ARGS__}, (Int)sizeof((Byte[]){__VA_ARGS__}),                \
             (Int)sizeof((Byte[]){__VA_ARGS__}), TYPE_BYTE})
#define NO_BYTES ((Slice){(Byte *)(uintptr_t)"", 0, 0, TYPE_BYTE})

static bool same_bytes(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : cstr("<nil>");
}

static Slice unhex(Alloc *a, const char *h) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, cstr(h), &err);
    if (BURROW_FAILED(err))
        return slice_nil(TYPE_BYTE);
    return b;
}

static Str tohex(Alloc *a, Slice b) {
    return hex_encode_to_string(a, b);
}

static bool same_zone(Time x, Time y) {
    Int xo = 0;
    Int yo = 0;
    Str xn = time_zone(x, &xo);
    Str yn = time_zone(y, &yo);
    return xo == yo && str_eq(xn, yn);
}

/* reflect.DeepEqual over the descriptors, for the kinds these tests use. A
 * Time is equal when it is the same instant in the same zone, and a *big.Int
 * when it holds the same number. */
static bool deep_equal(const Type *t, const void *x, const void *y) {
    if (t == TYPE_TIME) {
        Time tx = *(const Time *)x;
        Time ty = *(const Time *)y;
        return time_equal(tx, ty) && same_zone(tx, ty);
    }
    if (t->kind == KIND_POINTER && t->elem == TYPE_OF(BigInt)) {
        const BigInt *bx = *(BigInt *const *)x;
        const BigInt *by = *(BigInt *const *)y;
        if (bx == NULL || by == NULL)
            return bx == by;
        return big_int_cmp(bx, by) == 0;
    }
    switch ((int)t->kind) {
    case KIND_STRUCT:
        for (uint16_t i = 0; i < t->nfield; i++) {
            const Field *f = &t->fields[i];
            if (!deep_equal(f->type, (const Byte *)x + f->offset,
                            (const Byte *)y + f->offset))
                return false;
        }
        return true;
    case KIND_SLICE: {
        const Slice *sx = (const Slice *)x;
        const Slice *sy = (const Slice *)y;
        if ((sx->p == NULL) != (sy->p == NULL) || sx->len != sy->len)
            return false;
        for (Int i = 0; i < sx->len; i++)
            if (!deep_equal(t->elem, (const Byte *)sx->p + (size_t)i * t->elem->size,
                            (const Byte *)sy->p + (size_t)i * t->elem->size))
                return false;
        return true;
    }
    case KIND_STRING:
        return str_eq(*(const Str *)x, *(const Str *)y);
    case KIND_INTERFACE: {
        const Any *ax = (const Any *)x;
        const Any *ay = (const Any *)y;
        if (ax->t == NULL || ay->t == NULL)
            return ax->t == ay->t;
        return ax->t == ay->t && deep_equal(ax->t, ax->data, ay->data);
    }
    default:
        return memcmp(x, y, t->size) == 0;
    }
}

static Slice oid_slice(Alloc *a, const Int *v, Int n) {
    Slice s = slice_make(a, TYPE_INT, n, n);
    if (n > 0)
        memcpy(s.p, v, (size_t)n * sizeof(Int));
    return s;
}

/* An allocator that gives out a fixed number of allocations and then fails,
 * keeping count of what is still live. */
typedef struct Budget {
    Alloc *inner;
    long long left;
    long long live;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *p = mem_alloc(b->inner, size, align);
    if (p != NULL)
        b->live += (long long)size;
    return p;
}

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *q = mem_realloc(b->inner, p, old, nsz, align);
    if (q != NULL)
        b->live += (long long)nsz - (long long)old;
    return q;
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    mem_free(b->inner, p, size, align);
    b->live -= (long long)size;
}

static const AllocVT budget_vt = {budget_alloc, NULL, budget_realloc,
                                  budget_free,  NULL, NULL};

/* ------------------------------------------------------------------ types */

BURROW_SLICE_TYPE(IntSlice, Int);
BURROW_SLICE_TYPE(StrSlice, Str);
BURROW_PTR_TYPE(BigIntPtr, BigInt);

#define INT_STRUCT_FIELDS(F, T) F(T, Int, A, "")
BURROW_STRUCT(IntStruct, INT_STRUCT_FIELDS);

#define TWO_INT_STRUCT_FIELDS(F, T)                                                    \
    F(T, Int, A, "")                                                                   \
    F(T, Int, B, "")
BURROW_STRUCT(TwoIntStruct, TWO_INT_STRUCT_FIELDS);

#define BIG_INT_STRUCT_FIELDS(F, T) F(T, BigIntPtr, A, "")
BURROW_STRUCT(BigIntStruct, BIG_INT_STRUCT_FIELDS);

#define NESTED_STRUCT_FIELDS(F, T) F(T, IntStruct, A, "")
BURROW_STRUCT(NestedStruct, NESTED_STRUCT_FIELDS);

#define RAW_CONTENTS_STRUCT_FIELDS(F, T)                                               \
    F(T, Asn1RawContent, Raw, "")                                                      \
    F(T, Int, A, "")
BURROW_STRUCT(RawContentsStruct, RAW_CONTENTS_STRUCT_FIELDS);

#define IMPLICIT_TAG_TEST_FIELDS(F, T) F(T, Int, A, "asn1:\"implicit,tag:5\"")
BURROW_STRUCT(ImplicitTagTest, IMPLICIT_TAG_TEST_FIELDS);

#define EXPLICIT_TAG_TEST_FIELDS(F, T) F(T, Int, A, "asn1:\"explicit,tag:5\"")
BURROW_STRUCT(ExplicitTagTest, EXPLICIT_TAG_TEST_FIELDS);

#define FLAG_TEST_FIELDS(F, T) F(T, Asn1Flag, A, "asn1:\"tag:0,optional\"")
BURROW_STRUCT(FlagTest, FLAG_TEST_FIELDS);

#define GENERALIZED_TIME_TEST_FIELDS(F, T) F(T, Time, A, "asn1:\"generalized\"")
BURROW_STRUCT(GeneralizedTimeTest, GENERALIZED_TIME_TEST_FIELDS);

#define IA5_STRING_TEST_FIELDS(F, T) F(T, Str, A, "asn1:\"ia5\"")
BURROW_STRUCT(Ia5StringTest, IA5_STRING_TEST_FIELDS);

#define PRINTABLE_STRING_TEST_FIELDS(F, T) F(T, Str, A, "asn1:\"printable\"")
BURROW_STRUCT(PrintableStringTest, PRINTABLE_STRING_TEST_FIELDS);

#define GENERIC_STRING_TEST_FIELDS(F, T) F(T, Str, A, "")
BURROW_STRUCT(GenericStringTest, GENERIC_STRING_TEST_FIELDS);

#define OPTIONAL_RAW_VALUE_TEST_FIELDS(F, T) F(T, Asn1RawValue, A, "asn1:\"optional\"")
BURROW_STRUCT(OptionalRawValueTest, OPTIONAL_RAW_VALUE_TEST_FIELDS);

#define OMIT_EMPTY_TEST_FIELDS(F, T) F(T, StrSlice, A, "asn1:\"omitempty\"")
BURROW_STRUCT(OmitEmptyTest, OMIT_EMPTY_TEST_FIELDS);

#define DEFAULT_TEST_FIELDS(F, T) F(T, Int, A, "asn1:\"optional,default:1\"")
BURROW_STRUCT(DefaultTest, DEFAULT_TEST_FIELDS);

#define APPLICATION_TEST_FIELDS(F, T)                                                  \
    F(T, Int, A, "asn1:\"application,tag:0\"")                                         \
    F(T, Int, B, "asn1:\"application,tag:1,explicit\"")
BURROW_STRUCT(ApplicationTest, APPLICATION_TEST_FIELDS);

#define PRIVATE_TEST_FIELDS(F, T)                                                      \
    F(T, Int, A, "asn1:\"private,tag:0\"")                                             \
    F(T, Int, B, "asn1:\"private,tag:1,explicit\"")                                    \
    F(T, Int, C, "asn1:\"private,tag:31\"")                                            \
    F(T, Int, D, "asn1:\"private,tag:128\"")
BURROW_STRUCT(PrivateTest, PRIVATE_TEST_FIELDS);

#define NUMERIC_STRING_TEST_FIELDS(F, T) F(T, Str, A, "asn1:\"numeric\"")
BURROW_STRUCT(NumericStringTest, NUMERIC_STRING_TEST_FIELDS);

BURROW_NAMED_SLICE_TYPE(testSET, Int);

#define TEST_OBJECT_IDENTIFIER_STRUCT_FIELDS(F, T) F(T, Asn1ObjectIdentifier, OID, "")
BURROW_STRUCT(TestObjectIdentifierStruct, TEST_OBJECT_IDENTIFIER_STRUCT_FIELDS);

#define TEST_CONTEXT_SPECIFIC_TAGS_FIELDS(F, T) F(T, Int, A, "asn1:\"tag:1\"")
BURROW_STRUCT(TestContextSpecificTags, TEST_CONTEXT_SPECIFIC_TAGS_FIELDS);

#define TEST_CONTEXT_SPECIFIC_TAGS2_FIELDS(F, T)                                       \
    F(T, Int, A, "asn1:\"explicit,tag:1\"")                                            \
    F(T, Int, B, "")
BURROW_STRUCT(TestContextSpecificTags2, TEST_CONTEXT_SPECIFIC_TAGS2_FIELDS);

#define TEST_CONTEXT_SPECIFIC_TAGS3_FIELDS(F, T) F(T, Str, S, "asn1:\"tag:1,utf8\"")
BURROW_STRUCT(TestContextSpecificTags3, TEST_CONTEXT_SPECIFIC_TAGS3_FIELDS);

#define TEST_ELEMENTS_AFTER_STRING_FIELDS(F, T)                                        \
    F(T, Str, S, "")                                                                   \
    F(T, Int, A, "")                                                                   \
    F(T, Int, B, "")
BURROW_STRUCT(TestElementsAfterString, TEST_ELEMENTS_AFTER_STRING_FIELDS);

#define TEST_BIG_INT_FIELDS(F, T) F(T, BigIntPtr, X, "")
BURROW_STRUCT(TestBigInt, TEST_BIG_INT_FIELDS);

#define TEST_SET_FIELDS(F, T) F(T, IntSlice, Ints, "asn1:\"set\"")
BURROW_STRUCT(TestSet, TEST_SET_FIELDS);

#define ATTRIBUTE_TYPE_AND_VALUE_FIELDS(F, T)                                          \
    F(T, Asn1ObjectIdentifier, Type, "")                                               \
    F(T, Any, Value, "")
BURROW_STRUCT(AttributeTypeAndValue, ATTRIBUTE_TYPE_AND_VALUE_FIELDS);

BURROW_NAMED_SLICE_TYPE(RelativeDistinguishedNameSET, AttributeTypeAndValue);
BURROW_NAMED_SLICE_TYPE(RDNSequence, RelativeDistinguishedNameSET);

#define ALGORITHM_IDENTIFIER_FIELDS(F, T) F(T, Asn1ObjectIdentifier, Algorithm, "")
BURROW_STRUCT(AlgorithmIdentifier, ALGORITHM_IDENTIFIER_FIELDS);

#define VALIDITY_FIELDS(F, T)                                                          \
    F(T, Time, NotBefore, "")                                                          \
    F(T, Time, NotAfter, "")
BURROW_STRUCT(Validity, VALIDITY_FIELDS);

#define PUBLIC_KEY_INFO_FIELDS(F, T)                                                   \
    F(T, AlgorithmIdentifier, Algorithm, "")                                           \
    F(T, Asn1BitString, PublicKey, "")
BURROW_STRUCT(PublicKeyInfo, PUBLIC_KEY_INFO_FIELDS);

#define TBS_CERTIFICATE_FIELDS(F, T)                                                   \
    F(T, Int, Version, "asn1:\"optional,explicit,default:0,tag:0\"")                   \
    F(T, Asn1RawValue, SerialNumber, "")                                               \
    F(T, AlgorithmIdentifier, SignatureAlgorithm, "")                                  \
    F(T, RDNSequence, Issuer, "")                                                      \
    F(T, Validity, Validity, "")                                                       \
    F(T, RDNSequence, Subject, "")                                                     \
    F(T, PublicKeyInfo, PublicKey, "")
BURROW_STRUCT(TBSCertificate, TBS_CERTIFICATE_FIELDS);

#define CERTIFICATE_FIELDS(F, T)                                                       \
    F(T, TBSCertificate, TBSCertificate, "")                                           \
    F(T, AlgorithmIdentifier, SignatureAlgorithm, "")                                  \
    F(T, Asn1BitString, SignatureValue, "")
BURROW_STRUCT(Certificate, CERTIFICATE_FIELDS);

#define RAW_STRUCT_TEST_FIELDS(F, T)                                                   \
    F(T, Asn1RawContent, Raw, "")                                                      \
    F(T, Int, A, "")
BURROW_STRUCT(RawStructTest, RAW_STRUCT_TEST_FIELDS);

#define EXPLICIT_TAGGED_TIME_TEST_FIELDS(F, T)                                         \
    F(T, Time, Time, "asn1:\"explicit,tag:0\"")
BURROW_STRUCT(ExplicitTaggedTimeTest, EXPLICIT_TAGGED_TIME_TEST_FIELDS);

#define IMPLICIT_TAGGED_TIME_TEST_FIELDS(F, T) F(T, Time, Time, "asn1:\"tag:24\"")
BURROW_STRUCT(ImplicitTaggedTimeTest, IMPLICIT_TAGGED_TIME_TEST_FIELDS);

#define TRUNCATED_EXPLICIT_TAG_TEST_FIELDS(F, T)                                       \
    F(T, Int, Test, "asn1:\"explicit,tag:0\"")
BURROW_STRUCT(TruncatedExplicitTagTest, TRUNCATED_EXPLICIT_TAG_TEST_FIELDS);

#define INVALID_UTF8_TEST_FIELDS(F, T) F(T, Str, Str, "asn1:\"utf8\"")
BURROW_STRUCT(InvalidUTF8Test, INVALID_UTF8_TEST_FIELDS);

#define NIL_VALUE_FIELDS(F, T) F(T, Any, V, "")
BURROW_STRUCT(NilValue, NIL_VALUE_FIELDS);

#define UNEXPORTED_FIELDS(F, T)                                                        \
    F(T, Int, X, "")                                                                   \
    F(T, Int, y, "")
BURROW_STRUCT(Unexported, UNEXPORTED_FIELDS);

#define EXPORTED_FIELDS(F, T)                                                          \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT(Exported, EXPORTED_FIELDS);

#define FOO_FIELDS(F, T)                                                               \
    F(T, Asn1RawValue, A, "asn1:\"optional,explicit,tag:5\"")                          \
    F(T, Bytes, B, "asn1:\"optional,explicit,tag:6\"")
BURROW_STRUCT(Foo, FOO_FIELDS);

#define TAGGED_RAW_VALUE_FIELDS(F, T) F(T, Asn1RawValue, A, "asn1:\"tag:5\"")
BURROW_STRUCT(TaggedRawValue, TAGGED_RAW_VALUE_FIELDS);

#define UNTAGGED_RAW_VALUE_FIELDS(F, T) F(T, Asn1RawValue, A, "")
BURROW_STRUCT(UntaggedRawValue, UNTAGGED_RAW_VALUE_FIELDS);

#define TAGGED_FIELDS(F, T)                                                            \
    F(T, Str, IA5, "asn1:\"tag:1,ia5\"")                                               \
    F(T, Str, Printable, "asn1:\"tag:2,printable\"")                                   \
    F(T, Str, UTF8, "asn1:\"tag:3,utf8\"")                                             \
    F(T, Str, Numeric, "asn1:\"tag:4,numeric\"")                                       \
    F(T, Time, UTC, "asn1:\"tag:5,utc\"")                                              \
    F(T, Time, Generalized, "asn1:\"tag:6,generalized\"")
BURROW_STRUCT(Tagged, TAGGED_FIELDS);

#define BOMB_ELEM_FIELDS(F, T)                                                         \
    F(T, IntSlice, Id, "")                                                             \
    F(T, bool, Critical, "asn1:\"optional\"")                                          \
    F(T, Bytes, Value, "")
BURROW_STRUCT(BombElem, BOMB_ELEM_FIELDS);
BURROW_SLICE_TYPE(BombElemSlice, BombElem);

#define SET_STRINGS_FIELDS(F, T) F(T, StrSlice, Strings, "asn1:\"set\"")
BURROW_STRUCT(SetStrings, SET_STRINGS_FIELDS);

BURROW_NAMED_SLICE_TYPE(testSetSET, Str);

/* type Recursive []Recursive. */
BURROW_SLICE_TYPE(Recursive, Recursive);

/* type Recursive struct { Next []Recursive `asn1:"optional"` }. */
BURROW_SLICE_TYPE_DECL(RecursiveStructSlice, RecursiveStruct);
#define RECURSIVE_STRUCT_FIELDS(F, T)                                                  \
    F(T, RecursiveStructSlice, Next, "asn1:\"optional\"")
BURROW_STRUCT(RecursiveStruct, RECURSIVE_STRUCT_FIELDS);
BURROW_SLICE_TYPE(RecursiveStructSlice, RecursiveStruct);

/* ------------------------------------------------------------ the parsers */

static void TestParseBool(TestingT *t) {
    static const struct {
        Byte in[2];
        Int n;
        bool ok;
        bool out;
    } tests[] = {
        {{0x00}, 1, true, false},        {{0xff}, 1, true, true},
        {{0x00, 0x00}, 2, false, false}, {{0xff, 0xff}, 2, false, false},
        {{0x01}, 1, false, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        bool ret =
            burrow__asn1_parse_bool(lit((const char *)tests[i].in, tests[i].n), &err);
        if (BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did fail? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), tests[i].ok);
        if (tests[i].ok && ret != tests[i].out)
            testing_t_errorf_v(t, "#%d: Bad result: %t (expected %t)", (int64_t)i, ret,
                               tests[i].out);
    }
}

typedef struct IntCase {
    Byte in[9];
    Int n;
    bool ok;
    int64_t out;
} IntCase;

static void TestParseInt64(TestingT *t) {
    static const IntCase tests[] = {
        {{0x00}, 1, true, 0},
        {{0x7f}, 1, true, 127},
        {{0x00, 0x80}, 2, true, 128},
        {{0x01, 0x00}, 2, true, 256},
        {{0x80}, 1, true, -128},
        {{0xff, 0x7f}, 2, true, -129},
        {{0xff}, 1, true, -1},
        {{0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, true, INT64_MIN},
        {{0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 9, false, 0},
        {{0}, 0, false, 0},
        {{0x00, 0x7f}, 2, false, 0},
        {{0xff, 0xf0}, 2, false, 0},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        int64_t ret =
            burrow__asn1_parse_int64(lit((const char *)tests[i].in, tests[i].n), &err);
        if (BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did fail? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), tests[i].ok);
        if (tests[i].ok && ret != tests[i].out)
            testing_t_errorf_v(t, "#%d: Bad result: %d (expected %d)", (int64_t)i, ret,
                               tests[i].out);
    }
}

static void TestParseInt32(TestingT *t) {
    static const IntCase tests[] = {
        {{0x00}, 1, true, 0},
        {{0x7f}, 1, true, 127},
        {{0x00, 0x80}, 2, true, 128},
        {{0x01, 0x00}, 2, true, 256},
        {{0x80}, 1, true, -128},
        {{0xff, 0x7f}, 2, true, -129},
        {{0xff}, 1, true, -1},
        {{0x80, 0x00, 0x00, 0x00}, 4, true, -2147483648LL},
        {{0x80, 0x00, 0x00, 0x00, 0x00}, 5, false, 0},
        {{0}, 0, false, 0},
        {{0x00, 0x7f}, 2, false, 0},
        {{0xff, 0xf0}, 2, false, 0},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        int32_t ret =
            burrow__asn1_parse_int32(lit((const char *)tests[i].in, tests[i].n), &err);
        if (BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did fail? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), tests[i].ok);
        if (tests[i].ok && ret != tests[i].out)
            testing_t_errorf_v(t, "#%d: Bad result: %d (expected %d)", (int64_t)i, ret,
                               tests[i].out);
    }
}

static void TestParseBigInt(TestingT *t) {
    static const struct {
        Byte in[2];
        Int n;
        bool ok;
        const char *base10;
    } tests[] = {
        {{0xff}, 1, true, "-1"},
        {{0x00}, 1, true, "0"},
        {{0x01}, 1, true, "1"},
        {{0x00, 0xff}, 2, true, "255"},
        {{0xff, 0x00}, 2, true, "-256"},
        {{0x01, 0x00}, 2, true, "256"},
        {{0}, 0, false, ""},
        {{0x00, 0x7f}, 2, false, ""},
        {{0xff, 0xf0}, 2, false, ""},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice in = lit((const char *)tests[i].in, tests[i].n);
        Error err = BURROW_NO_ERROR;
        BigInt *ret = burrow__asn1_parse_big_int(a, in, &err);
        if (BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did fail? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), tests[i].ok);
        if (!tests[i].ok)
            continue;
        Str s = big_int_string(ret, a);
        if (!str_eq(s, cstr(tests[i].base10)))
            testing_t_errorf_v(t, "#%d: bad result from %s, got %s want %s", (int64_t)i,
                               tohex(a, in), s, cstr(tests[i].base10));
        Slice result = burrow__asn1_make_big_int(a, ret, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d: err=%q", (int64_t)i, err_str(err));
            continue;
        }
        if (!same_bytes(result, in))
            testing_t_errorf_v(t, "#%d: got %s from marshaling %s, want %s", (int64_t)i,
                               tohex(a, result), s, tohex(a, in));
    }
    arena_free(&ar);
}

static void TestBitString(TestingT *t) {
    static const struct {
        Byte in[2];
        Int n;
        bool ok;
        Byte out[1];
        Int out_n;
        Int bit_length;
    } tests[] = {
        {{0}, 0, false, {0}, 0, 0},
        {{0x00}, 1, true, {0}, 0, 0},
        {{0x07, 0x00}, 2, true, {0x00}, 1, 1},
        {{0x07, 0x01}, 2, false, {0}, 0, 0},
        {{0x07, 0x40}, 2, false, {0}, 0, 0},
        {{0x08, 0x00}, 2, false, {0}, 0, 0},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Asn1BitString ret = burrow__asn1_parse_bit_string(
            lit((const char *)tests[i].in, tests[i].n), &err);
        if (BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did fail? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), tests[i].ok);
        if (BURROW_OK(err) &&
            (tests[i].bit_length != ret.bit_length ||
             !same_bytes(ret.bytes, lit((const char *)tests[i].out, tests[i].out_n))))
            testing_t_errorf_v(t, "#%d: Bad result: %d bits (expected %d)", (int64_t)i,
                               ret.bit_length, tests[i].bit_length);
    }
}

static void TestBitStringAt(TestingT *t) {
    Asn1BitString bs = {BYTES(0x82, 0x40), 16};
    if (asn1_bit_string_at(bs, 0) != 1)
        testing_t_error_v(t, "#1: Failed");
    if (asn1_bit_string_at(bs, 1) != 0)
        testing_t_error_v(t, "#2: Failed");
    if (asn1_bit_string_at(bs, 6) != 1)
        testing_t_error_v(t, "#3: Failed");
    if (asn1_bit_string_at(bs, 9) != 1)
        testing_t_error_v(t, "#4: Failed");
    if (asn1_bit_string_at(bs, -1) != 0)
        testing_t_error_v(t, "#5: Failed");
    if (asn1_bit_string_at(bs, 17) != 0)
        testing_t_error_v(t, "#6: Failed");
}

static void TestBitStringRightAlign(TestingT *t) {
    static const struct {
        Byte in[2];
        Int n;
        Int inlen;
        Byte out[2];
    } tests[] = {
        {{0x80}, 1, 1, {0x01}},
        {{0x80, 0x80}, 2, 9, {0x01, 0x01}},
        {{0}, 0, 0, {0}},
        {{0xce}, 1, 8, {0xce}},
        {{0xce, 0x47}, 2, 16, {0xce, 0x47}},
        {{0x34, 0x50}, 2, 12, {0x03, 0x45}},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Asn1BitString bs = {lit((const char *)tests[i].in, tests[i].n), tests[i].inlen};
        Slice out = asn1_bit_string_right_align(bs, a);
        Slice want = lit((const char *)tests[i].out, tests[i].n);
        if (!same_bytes(out, want))
            testing_t_errorf_v(t, "#%d got: %s want: %s", (int64_t)i, tohex(a, out),
                               tohex(a, want));
    }
    arena_free(&ar);
}

static void TestObjectIdentifier(TestingT *t) {
    static const struct {
        Byte in[7];
        Int n;
        bool ok;
        Int out[4];
        Int out_n;
    } tests[] = {
        {{0}, 0, false, {0}, 0},
        {{85}, 1, true, {2, 5}, 2},
        {{85, 0x02}, 2, true, {2, 5, 2}, 3},
        {{85, 0x02, 0xc0, 0x00}, 4, true, {2, 5, 2, 0x2000}, 4},
        {{0x81, 0x34, 0x03}, 3, true, {2, 100, 3}, 3},
        {{85, 0x02, 0xc0, 0x80, 0x80, 0x80, 0x80}, 7, false, {0}, 0},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Asn1ObjectIdentifier ret = burrow__asn1_parse_object_identifier(
            a, lit((const char *)tests[i].in, tests[i].n), &err);
        if (BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did fail? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), tests[i].ok);
        if (BURROW_OK(err)) {
            Asn1ObjectIdentifier want = oid_slice(a, tests[i].out, tests[i].out_n);
            if (!deep_equal(TYPE_ASN1_OBJECT_IDENTIFIER, &want, &ret))
                testing_t_errorf_v(t, "#%d: Bad result: %s (expected %s)", (int64_t)i,
                                   asn1_object_identifier_string(ret, a),
                                   asn1_object_identifier_string(want, a));
        }
    }
    Str s = asn1_object_identifier_string(ASN1_OID(1, 2, 3, 4), a);
    if (!str_eq(s, cstr("1.2.3.4")))
        testing_t_errorf_v(t, "bad ObjectIdentifier.String(). Got %s, want 1.2.3.4", s);
    arena_free(&ar);
}

typedef struct TimeCase {
    const char *in;
    bool ok;
    Int year, month, day, hour, min, sec, nsec;
    bool utc;
    Int offset;
} TimeCase;

static Time time_case_want(Alloc *a, const TimeCase *c) {
    TimeLocation *loc =
        c->utc ? time_utc_loc : time_fixed_zone(a, BURROW_STR_EMPTY, c->offset);
    return time_date(c->year, (TimeMonth)c->month, c->day, c->hour, c->min, c->sec,
                     c->nsec, loc);
}

#define BAD_TIME(s) {s, false, 0, 0, 0, 0, 0, 0, 0, true, 0}

static void TestUTCTime(TestingT *t) {
    static const TimeCase tests[] = {
        {"910506164540-0700", true, 1991, 5, 6, 16, 45, 40, 0, false, -7 * 60 * 60},
        {"910506164540+0730", true, 1991, 5, 6, 16, 45, 40, 0, false,
         7 * 60 * 60 + 30 * 60},
        {"910506234540Z", true, 1991, 5, 6, 23, 45, 40, 0, true, 0},
        {"9105062345Z", true, 1991, 5, 6, 23, 45, 0, 0, true, 0},
        {"5105062345Z", true, 1951, 5, 6, 23, 45, 0, 0, true, 0},
        BAD_TIME("a10506234540Z"),
        BAD_TIME("91a506234540Z"),
        BAD_TIME("9105a6234540Z"),
        BAD_TIME("910506a34540Z"),
        BAD_TIME("910506334a40Z"),
        BAD_TIME("91050633444aZ"),
        BAD_TIME("910506334461Z"),
        BAD_TIME("910506334400Za"),
        /* These are invalid times. However, the time package normalises times
         * and they were accepted in some versions. See #11134. */
        BAD_TIME("000100000000Z"),
        BAD_TIME("101302030405Z"),
        BAD_TIME("100002030405Z"),
        BAD_TIME("100100030405Z"),
        BAD_TIME("100132030405Z"),
        BAD_TIME("100231030405Z"),
        BAD_TIME("100102240405Z"),
        BAD_TIME("100102036005Z"),
        BAD_TIME("100102030460Z"),
        BAD_TIME("-100102030410Z"),
        BAD_TIME("10-0102030410Z"),
        BAD_TIME("10-0002030410Z"),
        BAD_TIME("1001-02030410Z"),
        BAD_TIME("100102-030410Z"),
        BAD_TIME("10010203-0410Z"),
        BAD_TIME("1001020304-10Z"),
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const TimeCase *c = &tests[i];
        Error err = BURROW_NO_ERROR;
        Time ret = burrow__asn1_parse_utc_time(a, lit(c->in, (Int)strlen(c->in)), &err);
        if (BURROW_FAILED(err)) {
            if (c->ok)
                testing_t_errorf_v(t, "#%d: parseUTCTime(%q) = error %s", (int64_t)i,
                                   cstr(c->in), err_str(err));
            continue;
        }
        if (!c->ok) {
            testing_t_errorf_v(t, "#%d: parseUTCTime(%q) succeeded, should have failed",
                               (int64_t)i, cstr(c->in));
            continue;
        }
        /* Ignore the zone name, just the offset. */
        Str format = BURROW_S("Jan _2 15:04:05 -0700 2006");
        Str have = time_format(ret, a, format);
        Str want = time_format(time_case_want(a, c), a, format);
        if (!str_eq(have, want))
            testing_t_errorf_v(t, "#%d: parseUTCTime(%q) = %s, want %s", (int64_t)i,
                               cstr(c->in), have, want);
    }
    arena_free(&ar);
}

static void TestGeneralizedTime(TestingT *t) {
    static const TimeCase tests[] = {
        {"20100102030405Z", true, 2010, 1, 2, 3, 4, 5, 0, true, 0},
        BAD_TIME("20100102030405"),
        {"20100102030405.123456Z", true, 2010, 1, 2, 3, 4, 5, 123456000, true, 0},
        BAD_TIME("20100102030405.123456"),
        BAD_TIME("20100102030405.Z"),
        BAD_TIME("20100102030405."),
        {"20100102030405+0607", true, 2010, 1, 2, 3, 4, 5, 0, false,
         6 * 60 * 60 + 7 * 60},
        {"20100102030405-0607", true, 2010, 1, 2, 3, 4, 5, 0, false,
         -6 * 60 * 60 - 7 * 60},
        /* These are invalid times. However, the time package normalises times
         * and they were accepted in some versions. See #11134. */
        BAD_TIME("00000100000000Z"),
        BAD_TIME("20101302030405Z"),
        BAD_TIME("20100002030405Z"),
        BAD_TIME("20100100030405Z"),
        BAD_TIME("20100132030405Z"),
        BAD_TIME("20100231030405Z"),
        BAD_TIME("20100102240405Z"),
        BAD_TIME("20100102036005Z"),
        BAD_TIME("20100102030460Z"),
        BAD_TIME("-20100102030410Z"),
        BAD_TIME("2010-0102030410Z"),
        BAD_TIME("2010-0002030410Z"),
        BAD_TIME("201001-02030410Z"),
        BAD_TIME("20100102-030410Z"),
        BAD_TIME("2010010203-0410Z"),
        BAD_TIME("201001020304-10Z"),
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const TimeCase *c = &tests[i];
        Error err = BURROW_NO_ERROR;
        Time ret = burrow__asn1_parse_generalized_time(
            a, lit(c->in, (Int)strlen(c->in)), &err);
        if (BURROW_OK(err) != c->ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did fail? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), c->ok);
        if (BURROW_OK(err)) {
            Time want = time_case_want(a, c);
            if (!deep_equal(TYPE_TIME, &want, &ret))
                testing_t_errorf_v(t, "#%d: Bad result: %q → %s (expected %s)",
                                   (int64_t)i, cstr(c->in), time_string(ret, a),
                                   time_string(want, a));
        }
    }
    arena_free(&ar);
}

static void TestParseTagAndLength(TestingT *t) {
    static const struct {
        Byte in[7];
        Int n;
        bool ok;
        Asn1TagAndLength out;
    } tests[] = {
        {{0x80, 0x01}, 2, true, {2, 0, 1, false}},
        {{0xa0, 0x01}, 2, true, {2, 0, 1, true}},
        {{0x02, 0x00}, 2, true, {0, 2, 0, false}},
        {{0xfe, 0x00}, 2, true, {3, 30, 0, true}},
        {{0x1f, 0x1f, 0x00}, 3, true, {0, 31, 0, false}},
        {{0x1f, 0x81, 0x00, 0x00}, 4, true, {0, 128, 0, false}},
        {{0x1f, 0x81, 0x80, 0x01, 0x00}, 5, true, {0, 0x4001, 0, false}},
        {{0x00, 0x81, 0x80}, 3, true, {0, 0, 128, false}},
        {{0x00, 0x82, 0x01, 0x00}, 4, true, {0, 0, 256, false}},
        {{0x00, 0x83, 0x01, 0x00}, 4, false, {0, 0, 0, false}},
        {{0x1f, 0x85}, 2, false, {0, 0, 0, false}},
        {{0x30, 0x80}, 2, false, {0, 0, 0, false}},
        /* Superfluous zeros in the length should be an error. */
        {{0xa0, 0x82, 0x00, 0xff}, 4, false, {0, 0, 0, false}},
        /* Lengths up to the maximum size of an int should work. */
        {{0xa0, 0x84, 0x7f, 0xff, 0xff, 0xff}, 6, true, {2, 0, 0x7fffffff, true}},
        /* Lengths that would overflow an int should be rejected. */
        {{0xa0, 0x84, 0x80, 0x00, 0x00, 0x00}, 6, false, {0, 0, 0, false}},
        /* Long length form may not be used for lengths that fit in short form. */
        {{0xa0, 0x81, 0x7f}, 3, false, {0, 0, 0, false}},
        /* Tag numbers which would overflow int32 are rejected. (The value below
         * is 2^31.) */
        {{0x1f, 0x88, 0x80, 0x80, 0x80, 0x00, 0x00}, 7, false, {0, 0, 0, false}},
        /* Tag numbers that fit in an int32 are valid. (The value below is
         * 2^31 - 1.) */
        {{0x1f, 0x87, 0xFF, 0xFF, 0xFF, 0x7F, 0x00}, 7, true, {0, INT32_MAX, 0, false}},
        /* Long tag number form may not be used for tags that fit in short
         * form. */
        {{0x1f, 0x1e, 0x00}, 3, false, {0, 0, 0, false}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Asn1TagAndLength got;
        Error err = BURROW_NO_ERROR;
        burrow__asn1_parse_tag_and_length(lit((const char *)tests[i].in, tests[i].n), 0,
                                          &got, &err);
        if (BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(
                t, "#%d: Incorrect error result (did pass? %t, expected: %t)",
                (int64_t)i, BURROW_OK(err), tests[i].ok);
        const Asn1TagAndLength *w = &tests[i].out;
        if (BURROW_OK(err) &&
            (got.cls != w->cls || got.tag != w->tag || got.length != w->length ||
             got.is_compound != w->is_compound))
            testing_t_errorf_v(
                t, "#%d: Bad result: {%d %d %d %t} (expected {%d %d %d %t})",
                (int64_t)i, got.cls, got.tag, got.length, got.is_compound, w->cls,
                w->tag, w->length, w->is_compound);
    }
}

static void TestParseFieldParameters(TestingT *t) {
    typedef Asn1FieldParameters P;
    static const struct {
        const char *in;
        P out;
    } tests[] = {
        {"", {0}},
        {"ia5", {.string_type = ASN1_TAG_IA5_STRING}},
        {"generalized", {.time_type = ASN1_TAG_GENERALIZED_TIME}},
        {"utc", {.time_type = ASN1_TAG_UTC_TIME}},
        {"printable", {.string_type = ASN1_TAG_PRINTABLE_STRING}},
        {"numeric", {.string_type = ASN1_TAG_NUMERIC_STRING}},
        {"optional", {.optional = true}},
        {"explicit", {.is_explicit = true, .has_tag = true}},
        {"application", {.application = true, .has_tag = true}},
        {"private", {.is_private = true, .has_tag = true}},
        {"optional,explicit", {.optional = true, .is_explicit = true, .has_tag = true}},
        {"default:42", {.has_default = true, .default_value = 42}},
        {"tag:17", {.has_tag = true, .tag = 17}},
        {"optional,explicit,default:42,tag:17",
         {.optional = true,
          .is_explicit = true,
          .has_default = true,
          .default_value = 42,
          .has_tag = true,
          .tag = 17}},
        {"optional,explicit,default:42,tag:17,rubbish1",
         {.optional = true,
          .is_explicit = true,
          .has_default = true,
          .default_value = 42,
          .has_tag = true,
          .tag = 17}},
        {"set", {.set = true}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        P f = burrow__asn1_parse_field_parameters(cstr(tests[i].in));
        const P *w = &tests[i].out;
        if (f.optional != w->optional || f.is_explicit != w->is_explicit ||
            f.application != w->application || f.is_private != w->is_private ||
            f.has_default != w->has_default || f.default_value != w->default_value ||
            f.has_tag != w->has_tag || f.tag != w->tag ||
            f.string_type != w->string_type || f.time_type != w->time_type ||
            f.set != w->set || f.omit_empty != w->omit_empty)
            testing_t_errorf_v(t, "#%d: Bad result for %q", (int64_t)i,
                               cstr(tests[i].in));
    }
}

/* ------------------------------------------------------------ unmarshal */

/* The table of TestUnmarshal, which BenchmarkUnmarshal runs too when b is
 * not NULL. */
static void unmarshal_table(TestingT *t, TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        Slice in;
        Any out;
    } tests[] = {
        {BYTES(0x02, 0x01, 0x42), BURROW_ANY(TYPE_INT, &(Int){0x42})},
        {BYTES(0x05, 0x00),
         BURROW_ANY(TYPE_ASN1_RAW_VALUE,
                    (&(Asn1RawValue){0, 5, false, NO_BYTES, BYTES(0x05, 0x00)}))},
        {BYTES(0x30, 0x08, 0x06, 0x06, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d),
         BURROW_ANY(TYPE_OF(TestObjectIdentifierStruct),
                    (&(TestObjectIdentifierStruct){ASN1_OID(1, 2, 840, 113549)}))},
        {BYTES(0x03, 0x04, 0x06, 0x6e, 0x5d, 0xc0),
         BURROW_ANY(TYPE_ASN1_BIT_STRING, (&(Asn1BitString){BYTES(110, 93, 192), 18}))},
        {BYTES(0x30, 0x09, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02, 0x02, 0x01, 0x03),
         BURROW_ANY(TYPE_OF(IntSlice), (&(Slice){(Int[]){1, 2, 3}, 3, 3, TYPE_INT}))},
        {BYTES(0x02, 0x01, 0x10), BURROW_ANY(TYPE_INT, &(Int){16})},
        {BYTES(0x13, 0x04, 't', 'e', 's', 't'),
         BURROW_ANY(TYPE_STRING, (Str[]){BURROW_S("test")})},
        {BYTES(0x16, 0x04, 't', 'e', 's', 't'),
         BURROW_ANY(TYPE_STRING, (Str[]){BURROW_S("test")})},
        /* Ampersand is allowed in PrintableString due to mistakes by major
         * CAs. */
        {BYTES(0x13, 0x05, 't', 'e', 's', 't', '&'),
         BURROW_ANY(TYPE_STRING, (Str[]){BURROW_S("test&")})},
        {BYTES(0x16, 0x04, 't', 'e', 's', 't'),
         BURROW_ANY(TYPE_ASN1_RAW_VALUE,
                    (&(Asn1RawValue){0, 22, false, LIT("test"), LIT("\x16\x04test")}))},
        {BYTES(0x04, 0x04, 1, 2, 3, 4),
         BURROW_ANY(TYPE_ASN1_RAW_VALUE,
                    (&(Asn1RawValue){0, 4, false, BYTES(1, 2, 3, 4),
                                     BYTES(4, 4, 1, 2, 3, 4)}))},
        {BYTES(0x30, 0x03, 0x81, 0x01, 0x01),
         BURROW_ANY(TYPE_OF(TestContextSpecificTags), (&(TestContextSpecificTags){1}))},
        {BYTES(0x30, 0x08, 0xa1, 0x03, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02),
         BURROW_ANY(TYPE_OF(TestContextSpecificTags2),
                    (&(TestContextSpecificTags2){1, 2}))},
        {BYTES(0x30, 0x03, 0x81, 0x01, '@'),
         BURROW_ANY(TYPE_OF(TestContextSpecificTags3),
                    (&(TestContextSpecificTags3){BURROW_S("@")}))},
        {BYTES(0x01, 0x01, 0x00), BURROW_ANY(TYPE_BOOL, &(bool){false})},
        {BYTES(0x01, 0x01, 0xff), BURROW_ANY(TYPE_BOOL, &(bool){true})},
        {BYTES(0x30, 0x0b, 0x13, 0x03, 0x66, 0x6f, 0x6f, 0x02, 0x01, 0x22, 0x02, 0x01,
               0x33),
         BURROW_ANY(TYPE_OF(TestElementsAfterString),
                    (&(TestElementsAfterString){BURROW_S("foo"), 0x22, 0x33}))},
        {BYTES(0x30, 0x05, 0x02, 0x03, 0x12, 0x34, 0x56),
         BURROW_ANY(TYPE_OF(TestBigInt), (&(TestBigInt){big_new_int(a, 0x123456)}))},
        {BYTES(0x30, 0x0b, 0x31, 0x09, 0x02, 0x01, 0x01, 0x02, 0x01, 0x02, 0x02, 0x01,
               0x03),
         BURROW_ANY(TYPE_OF(TestSet),
                    (&(TestSet){{(Int[]){1, 2, 3}, 3, 3, TYPE_INT}}))},
        {BYTES(0x12, 0x0b, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ' '),
         BURROW_ANY(TYPE_STRING, (Str[]){BURROW_S("0123456789 ")})},
        {BYTES(0x14, 0x03, 0xbf, 0x61, 0x3f),
         BURROW_ANY(TYPE_STRING, (Str[]){BURROW_S("\xc2\xbf"
                                                  "a?")})},
    };
    if (b != NULL) {
        void *vals[sizeof tests / sizeof tests[0]];
        for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
            vals[i] = mem_alloc(a, tests[i].out.t->size, tests[i].out.t->align);
        ArenaMark m = arena_mark(&ar);
        testing_b_report_allocs(b);
        testing_b_reset_timer(b);
        for (Int n = 0; n < testing_b_n(b); n++) {
            for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
                Error err = BURROW_NO_ERROR;
                asn1_unmarshal(a, tests[i].in, BURROW_ANY(tests[i].out.t, vals[i]),
                               &err);
            }
            arena_release(&ar, m);
        }
        arena_free(&ar);
        return;
    }
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const Type *ty = tests[i].out.t;
        void *val = mem_alloc(a, ty->size, ty->align);
        Error err = BURROW_NO_ERROR;
        asn1_unmarshal(a, tests[i].in, BURROW_ANY(ty, val), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Unmarshal failed at index %d %s", (int64_t)i,
                               err_str(err));
        if (!deep_equal(ty, val, tests[i].out.data))
            testing_t_errorf_v(t, "#%d: have and want differ", (int64_t)i);
    }
    arena_free(&ar);
}

static void TestUnmarshal(TestingT *t) {
    unmarshal_table(t, NULL);
}

static void BenchmarkUnmarshal(TestingB *b) {
    unmarshal_table(NULL, b);
}

static void TestUnmarshalWithNilOrNonPointer(TestingT *t) {
    const struct {
        Any v;
        const char *want;
    } tests[] = {
        {{NULL, NULL}, "asn1: Unmarshal recipient value is nil"},
        {BURROW_ANY(TYPE_ASN1_RAW_VALUE, NULL),
         "asn1: Unmarshal recipient value is nil *asn1.RawValue"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        asn1_unmarshal(heap_allocator(), BYTES(0x05, 0x00), tests[i].v, &err);
        if (BURROW_OK(err)) {
            testing_t_error_v(t, "Unmarshal expecting error, got nil");
            continue;
        }
        if (!str_eq(error_text(err), cstr(tests[i].want)))
            testing_t_errorf_v(t, "InvalidUnmarshalError mismatch\nGot:  %q\nWant: %q",
                               error_text(err), cstr(tests[i].want));
    }
}

/* derEncodedSelfSignedCert. */
static RDNSequence cert_names(Alloc *a) {
    static const struct {
        Int oid[7];
        Int n;
        const char *value;
    } names[] = {
        {{2, 5, 4, 6}, 4, "XX"},
        {{2, 5, 4, 8}, 4, "Some-State"},
        {{2, 5, 4, 7}, 4, "City"},
        {{2, 5, 4, 10}, 4, "Internet Widgits Pty Ltd"},
        {{2, 5, 4, 3}, 4, "false.example.com"},
        {{1, 2, 840, 113549, 1, 9, 1}, 7, "false@example.com"},
    };
    Int n = (Int)(sizeof names / sizeof names[0]);
    RDNSequence seq = slice_make(a, TYPE_OF(RelativeDistinguishedNameSET), n, n);
    for (Int i = 0; i < n; i++) {
        RelativeDistinguishedNameSET set =
            slice_make(a, TYPE_OF(AttributeTypeAndValue), 1, 1);
        AttributeTypeAndValue *atv = (AttributeTypeAndValue *)set.p;
        atv->Type = oid_slice(a, names[i].oid, names[i].n);
        Str *s = mem_alloc(a, sizeof(Str), _Alignof(Str));
        *s = cstr(names[i].value);
        atv->Value = BURROW_ANY(TYPE_STRING, s);
        ((RelativeDistinguishedNameSET *)seq.p)[i] = set;
    }
    return seq;
}

static Certificate self_signed_cert(Alloc *a) {
    static const Byte public_key[] = {
        0x30, 0x48, 0x2,  0x41, 0x0,  0xcd, 0xb7, 0x63, 0x9c, 0x32, 0x78, 0xf0, 0x6,
        0xaa, 0x27, 0x7f, 0x6e, 0xaf, 0x42, 0x90, 0x2b, 0x59, 0x2d, 0x8c, 0xbc, 0xbe,
        0x38, 0xa1, 0xc9, 0x2b, 0xa4, 0x69, 0x5a, 0x33, 0x1b, 0x1d, 0xea, 0xde, 0xad,
        0xd8, 0xe9, 0xa5, 0xc2, 0x7e, 0x8c, 0x4c, 0x2f, 0xd0, 0xa8, 0x88, 0x96, 0x57,
        0x72, 0x2a, 0x4f, 0x2a, 0xf7, 0x58, 0x9c, 0xf2, 0xc7, 0x70, 0x45, 0xdc, 0x8f,
        0xde, 0xec, 0x35, 0x7d, 0x2,  0x3,  0x1,  0x0,  0x1,
    };
    static const Byte signature[] = {
        0xa6, 0x7b, 0x6,  0xec, 0x5e, 0xce, 0x92, 0x77, 0x2c, 0xa4, 0x13, 0xcb, 0xa3,
        0xca, 0x12, 0x56, 0x8f, 0xdc, 0x6c, 0x7b, 0x45, 0x11, 0xcd, 0x40, 0xa7, 0xf6,
        0x59, 0x98, 0x4,  0x2,  0xdf, 0x2b, 0x99, 0x8b, 0xb9, 0xa4, 0xa8, 0xcb, 0xeb,
        0x34, 0xc0, 0xf0, 0xa7, 0x8c, 0xf8, 0xd9, 0x1e, 0xde, 0x14, 0xa5, 0xed, 0x76,
        0xbf, 0x11, 0x6f, 0xe3, 0x60, 0xaa, 0xfa, 0x88, 0x21, 0x49, 0x4,  0x35,
    };
    static const Int sha1_rsa[] = {1, 2, 840, 113549, 1, 1, 5};
    static const Int rsa[] = {1, 2, 840, 113549, 1, 1, 1};
    Certificate c;
    memset(&c, 0, sizeof c);
    c.TBSCertificate.Version = 0;
    c.TBSCertificate.SerialNumber = (Asn1RawValue){
        0, 2, false, BYTES(0x0, 0x8c, 0xc3, 0x37, 0x92, 0x10, 0xec, 0x2c, 0x98),
        BYTES(2, 9, 0x0, 0x8c, 0xc3, 0x37, 0x92, 0x10, 0xec, 0x2c, 0x98)};
    /* BYTES makes a compound literal that lives as long as this function, so
     * the two raw slices are copied into a before it returns. */
    Asn1RawValue *sn = &c.TBSCertificate.SerialNumber;
    Slice bytes = slice_make(a, TYPE_BYTE, sn->bytes.len, sn->bytes.len);
    memcpy(bytes.p, sn->bytes.p, (size_t)bytes.len);
    Slice full = slice_make(a, TYPE_BYTE, sn->full_bytes.len, sn->full_bytes.len);
    memcpy(full.p, sn->full_bytes.p, (size_t)full.len);
    sn->bytes = bytes;
    sn->full_bytes = full;
    c.TBSCertificate.SignatureAlgorithm.Algorithm = oid_slice(a, sha1_rsa, 7);
    c.TBSCertificate.Issuer = cert_names(a);
    c.TBSCertificate.Validity.NotBefore =
        time_date(2009, 10, 8, 0, 25, 53, 0, time_utc_loc);
    c.TBSCertificate.Validity.NotAfter =
        time_date(2010, 10, 8, 0, 25, 53, 0, time_utc_loc);
    c.TBSCertificate.Subject = cert_names(a);
    c.TBSCertificate.PublicKey.Algorithm.Algorithm = oid_slice(a, rsa, 7);
    c.TBSCertificate.PublicKey.PublicKey =
        (Asn1BitString){lit((const char *)public_key, (Int)sizeof public_key), 592};
    c.SignatureAlgorithm.Algorithm = oid_slice(a, sha1_rsa, 7);
    c.SignatureValue =
        (Asn1BitString){lit((const char *)signature, (Int)sizeof signature), 512};
    return c;
}

static void TestCertificate(TestingT *t) {
    /* This is a minimal, self-signed certificate that should parse
     * correctly. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Certificate cert;
    memset(&cert, 0, sizeof cert);
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(
        a, lit((const char *)der_self_signed_cert, (Int)sizeof der_self_signed_cert),
        BURROW_ANY(TYPE_OF(Certificate), &cert), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unmarshal failed: %s", err_str(err));
    Certificate want = self_signed_cert(a);
    if (!deep_equal(TYPE_OF(Certificate), &cert, &want))
        testing_t_error_v(
            t, "Bad result: the certificate differs from the one Go expects");
    arena_free(&ar);
}

static void TestCertificateWithNUL(TestingT *t) {
    /* This is the paypal NUL-hack certificate. It should fail to parse
     * because NUL isn't a permitted character in a PrintableString. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Certificate cert;
    memset(&cert, 0, sizeof cert);
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(
        a, lit((const char *)der_paypal_nul_cert, (Int)sizeof der_paypal_nul_cert),
        BURROW_ANY(TYPE_OF(Certificate), &cert), &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "Unmarshal succeeded, should not have");
    arena_free(&ar);
}

static void TestRawStructs(TestingT *t) {
    RawStructTest s;
    memset(&s, 0, sizeof s);
    Slice input = BYTES(0x30, 0x03, 0x02, 0x01, 0x50);
    Error err = BURROW_NO_ERROR;
    Slice rest = asn1_unmarshal(heap_allocator(), input,
                                BURROW_ANY(TYPE_OF(RawStructTest), &s), &err);
    if (rest.len != 0) {
        testing_t_errorf_v(t, "incomplete parse: %s", tohex(error_allocator(), rest));
        return;
    }
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err_str(err));
        return;
    }
    if (s.A != 0x50)
        testing_t_errorf_v(t, "bad value for A: got %d want %d", s.A, (Int)0x50);
    if (!same_bytes(s.Raw, input))
        testing_t_errorf_v(t, "bad value for Raw: got %s want %s",
                           tohex(error_allocator(), s.Raw),
                           tohex(error_allocator(), input));
}

static void TestObjectIdentifierEqual(TestingT *t) {
    const struct {
        Asn1ObjectIdentifier first;
        Asn1ObjectIdentifier second;
        bool same;
    } tests[] = {
        {ASN1_OID(1, 2, 3), ASN1_OID(1, 2, 3), true},
        {ASN1_OID(1), ASN1_OID(1, 2, 3), false},
        {ASN1_OID(1, 2, 3), ASN1_OID(10, 11, 12), false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool s = asn1_object_identifier_equal(tests[i].first, tests[i].second);
        if (s != tests[i].same)
            testing_t_errorf_v(t, "ObjectIdentifier.Equal: got: %t want: %t", s,
                               tests[i].same);
    }
}

static void TestStringSlice(TestingT *t) {
    static const char *const tests[][2] = {
        {"foo", "bar"},
        {"foo", "\\bar"},
        {"foo", "\"bar\""},
        {"foo", "\xc3\xa5\xc3\xa4\xc3\xb6"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in[2] = {cstr(tests[i][0]), cstr(tests[i][1])};
        StrSlice test = {in, 2, 2, TYPE_STRING};
        Error err = BURROW_NO_ERROR;
        Slice bs = asn1_marshal(a, BURROW_ANY(TYPE_OF(StrSlice), &test), &err);
        if (BURROW_FAILED(err))
            testing_t_error_v(t, err_str(err));
        StrSlice res = slice_nil(TYPE_STRING);
        asn1_unmarshal(a, bs, BURROW_ANY(TYPE_OF(StrSlice), &res), &err);
        if (BURROW_FAILED(err))
            testing_t_error_v(t, err_str(err));
        if (!deep_equal(TYPE_OF(StrSlice), &res, &test))
            testing_t_errorf_v(t, "incorrect marshal/unmarshal of %q, %q", in[0],
                               in[1]);
    }
    arena_free(&ar);
}

static void TestExplicitTaggedTime(TestingT *t) {
    /* Test that a time.Time will match either tagUTCTime or
     * tagGeneralizedTime. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        Slice in;
        Time out;
    } tests[] = {
        {BYTES(0x30, 0x11, 0xa0, 0xf, 0x17, 0xd, '9', '1', '0', '5', '0', '6', '1', '6',
               '4', '5', '4', '0', 'Z'),
         time_date(1991, 5, 6, 16, 45, 40, 0, time_utc_loc)},
        {BYTES(0x30, 0x17, 0xa0, 0xf, 0x18, 0x13, '2', '0', '1', '0', '0', '1', '0',
               '2', '0', '3', '0', '4', '0', '5', '+', '0', '6', '0', '7'),
         time_date(2010, 1, 2, 3, 4, 5, 0,
                   time_fixed_zone(a, BURROW_STR_EMPTY, 6 * 60 * 60 + 7 * 60))},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ExplicitTaggedTimeTest got;
        memset(&got, 0, sizeof got);
        Error err = BURROW_NO_ERROR;
        asn1_unmarshal(a, tests[i].in,
                       BURROW_ANY(TYPE_OF(ExplicitTaggedTimeTest), &got), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Unmarshal failed at index %d %s", (int64_t)i,
                               err_str(err));
        if (!time_equal(got.Time, tests[i].out))
            testing_t_errorf_v(t, "#%d: got %s, want %s", (int64_t)i,
                               time_string(got.Time, a), time_string(tests[i].out, a));
    }
    arena_free(&ar);
}

static void TestImplicitTaggedTime(TestingT *t) {
    /* An implicitly tagged time value, that happens to have an implicit tag
     * equal to a GENERALIZEDTIME, should still be parsed as a UTCTime.
     * (There's no "timeType" in fieldParameters to determine what type of time
     * should be expected when implicitly tagged.) */
    Slice der = BYTES(0x30, 0x0f, 0x80 | 24, 0xd, '9', '1', '0', '5', '0', '6', '1',
                      '6', '4', '5', '4', '0', 'Z');
    ImplicitTaggedTimeTest result;
    memset(&result, 0, sizeof result);
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(heap_allocator(), der,
                   BURROW_ANY(TYPE_OF(ImplicitTaggedTimeTest), &result), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Error while parsing: %s", err_str(err));
    Time expected = time_date(1991, 5, 6, 16, 45, 40, 0, time_utc_loc);
    if (!time_equal(result.Time, expected))
        testing_t_errorf_v(t, "Wrong result. Got %s, want %s",
                           time_string(result.Time, error_allocator()),
                           time_string(expected, error_allocator()));
}

static void TestTruncatedExplicitTag(TestingT *t) {
    /* This crashed Unmarshal in the past. See #11154. */
    Slice der = BYTES(0x30, /* SEQUENCE */
                      0x02, /* two bytes long */
                      0xa0, /* context-specific, tag 0 */
                      0x30  /* 48 bytes long */
    );
    TruncatedExplicitTagTest result = {0};
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(heap_allocator(), der,
                   BURROW_ANY(TYPE_OF(TruncatedExplicitTagTest), &result), &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "Unmarshal returned without error");
}

static void TestUnmarshalInvalidUTF8(TestingT *t) {
    Slice data = LIT("0\x05\f\x03"
                     "a\xc9"
                     "c");
    InvalidUTF8Test result;
    memset(&result, 0, sizeof result);
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(heap_allocator(), data,
                   BURROW_ANY(TYPE_OF(InvalidUTF8Test), &result), &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "Successfully unmarshaled invalid UTF-8 data");
    else if (!strings_contains(error_text(err), BURROW_S("UTF")))
        testing_t_fatalf_v(t, "Expected error to mention %q but error was %q",
                           BURROW_S("UTF"), error_text(err));
}

static void TestMarshalNilValue(TestingT *t) {
    NilValue nv = {{NULL, NULL}};
    const Any tests[] = {
        {NULL, NULL},
        BURROW_ANY(TYPE_OF(NilValue), &nv),
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        asn1_marshal(heap_allocator(), tests[i], &err);
        if (BURROW_OK(err))
            testing_t_fatalf_v(t, "#%d: successfully marshaled nil value", (int64_t)i);
    }
}

static void TestUnexportedStructField(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error want = asn1_structural_error_as_error(
        (Asn1StructuralError){BURROW_S("struct contains unexported fields")}, a);

    Unexported u = {5, 1};
    Error err = BURROW_NO_ERROR;
    asn1_marshal(a, BURROW_ANY(TYPE_OF(Unexported), &u), &err);
    if (!errors_is(err, want))
        testing_t_errorf_v(t, "got %s, want %s", err_str(err), error_text(want));

    Exported e = {5, 1};
    Slice bs = asn1_marshal(a, BURROW_ANY(TYPE_OF(Exported), &e), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err_str(err));
    Unexported u2 = {0, 0};
    asn1_unmarshal(a, bs, BURROW_ANY(TYPE_OF(Unexported), &u2), &err);
    if (!errors_is(err, want))
        testing_t_errorf_v(t, "got %s, want %s", err_str(err), error_text(want));
    arena_free(&ar);
}

static void TestNull(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Asn1RawValue null = asn1_null_raw_value;
    Slice marshaled = asn1_marshal(a, BURROW_ANY(TYPE_ASN1_RAW_VALUE, &null), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err_str(err));
    if (!same_bytes(asn1_null_bytes, marshaled))
        testing_t_errorf_v(t, "Expected Marshal of NullRawValue to yield %s, got %s",
                           tohex(a, asn1_null_bytes), tohex(a, marshaled));

    Asn1RawValue unmarshaled;
    memset(&unmarshaled, 0, sizeof unmarshaled);
    asn1_unmarshal(a, asn1_null_bytes, BURROW_ANY(TYPE_ASN1_RAW_VALUE, &unmarshaled),
                   &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err_str(err));

    unmarshaled.full_bytes = asn1_null_raw_value.full_bytes;
    if (unmarshaled.bytes.len == 0) {
        /* DeepEqual considers a nil slice and an empty slice to be
         * different. */
        unmarshaled.bytes = asn1_null_raw_value.bytes;
    }
    if (!deep_equal(TYPE_ASN1_RAW_VALUE, &null, &unmarshaled))
        testing_t_error_v(t, "Expected Unmarshal of NullBytes to yield NullRawValue");
    arena_free(&ar);
}

static void TestExplicitTagRawValueStruct(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Foo before;
    memset(&before, 0, sizeof before);
    before.B = BYTES(1, 2, 3);
    Error err = BURROW_NO_ERROR;
    Slice der = asn1_marshal(a, BURROW_ANY(TYPE_OF(Foo), &before), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err_str(err));

    Foo after;
    memset(&after, 0, sizeof after);
    Slice rest = asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(Foo), &after), &err);
    if (BURROW_FAILED(err) || rest.len != 0)
        testing_t_fatal_v(t, err_str(err));

    if (!deep_equal(TYPE_OF(Foo), &after, &before))
        testing_t_errorf_v(t, "got and want differ (DER: %s)", tohex(a, der));
    arena_free(&ar);
}

static void TestTaggedRawValue(TestingT *t) {
    enum { IS_COMPOUND = 0x20, TAG = 5 };
    const struct {
        bool should_match;
        Slice der;
    } tests[] = {
        {false, BYTES(0x30, 3, ASN1_TAG_INTEGER, 1, 1)},
        {true, BYTES(0x30, 3, (ASN1_CLASS_CONTEXT_SPECIFIC << 6) | TAG, 1, 1)},
        {true,
         BYTES(0x30, 3, (ASN1_CLASS_CONTEXT_SPECIFIC << 6) | TAG | IS_COMPOUND, 1, 1)},
        {false,
         BYTES(0x30, 3, (ASN1_CLASS_APPLICATION << 6) | TAG | IS_COMPOUND, 1, 1)},
        {false, BYTES(0x30, 3, (ASN1_CLASS_PRIVATE << 6) | TAG | IS_COMPOUND, 1, 1)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TaggedRawValue tagged;
        memset(&tagged, 0, sizeof tagged);
        Error err = BURROW_NO_ERROR;
        asn1_unmarshal(heap_allocator(), tests[i].der,
                       BURROW_ANY(TYPE_OF(TaggedRawValue), &tagged), &err);
        if (BURROW_OK(err) != tests[i].should_match)
            testing_t_errorf_v(t, "#%d: unexpected result parsing %s: %s", (int64_t)i,
                               tohex(error_allocator(), tests[i].der), err_str(err));

        /* An untagged RawValue should accept anything. */
        UntaggedRawValue untagged;
        memset(&untagged, 0, sizeof untagged);
        asn1_unmarshal(heap_allocator(), tests[i].der,
                       BURROW_ANY(TYPE_OF(UntaggedRawValue), &untagged), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(
                t, "#%d: unexpected failure parsing %s with untagged RawValue: %s",
                (int64_t)i, tohex(error_allocator(), tests[i].der), err_str(err));
    }
}

static void TestBMPString(TestingT *t) {
    static const struct {
        const char *name;
        const char *decoded;
        const char *encoded_hex;
        bool invalid;
    } tests[] = {
        {"empty string", "", "0000", false},
        /* Example from https://tools.ietf.org/html/rfc7292#appendix-B. */
        {"rfc7292 example", "Beavis", "0042006500610076006900730000", false},
        /* Some characters from the "Letterlike Symbols Unicode block". */
        {"letterlike symbols", "\xe2\x84\x95 - Double-struck N",
         "21150020002d00200044006f00750062006c0065002d00730074007200750063006b0020004e0"
         "000",
         false},
        {"invalid length", "", "ff", true},
        {"invalid surrogate", "", "5051d801", true},
        {"invalid noncharacter 0xfdd1", "", "5051fdd1", true},
        {"invalid noncharacter 0xffff", "", "5051ffff", true},
        {"invalid noncharacter 0xfffe", "", "5051fffe", true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str name = cstr(tests[i].name);
        Slice encoded = unhex(a, tests[i].encoded_hex);
        Error err = BURROW_NO_ERROR;
        Str decoded = burrow__asn1_parse_bmp_string(a, encoded, &err);
        if (BURROW_FAILED(err) && !tests[i].invalid)
            testing_t_errorf_v(t, "%s: parseBMPString failed: %s", name, err_str(err));
        else if (tests[i].invalid && BURROW_OK(err))
            testing_t_errorf_v(t, "%s: parseBMPString didn't fail as expected", name);
        if (!str_eq(decoded, cstr(tests[i].decoded)))
            testing_t_errorf_v(t, "%s: parseBMPString(%s): got %q, want %q", name,
                               cstr(tests[i].encoded_hex), decoded,
                               cstr(tests[i].decoded));
    }
    arena_free(&ar);
}

static void TestNonMinimalEncodedOID(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice h = unhex(a, "060a2a80864886f70d01010b");
    Asn1ObjectIdentifier oid = slice_nil(TYPE_INT);
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(a, h, BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &oid), &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "accepted non-minimally encoded oid");
    arena_free(&ar);
}

static void TestImplicitTypeRoundtrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Tagged x = {
        BURROW_S("ia5"),
        BURROW_S("printable"),
        BURROW_S("utf8"),
        BURROW_S("123 456"),
        time_truncate(time_utc(time_now()), TIME_SECOND),
        time_truncate(time_utc(time_now()), TIME_SECOND),
    };
    Error err = BURROW_NO_ERROR;
    Slice enc = asn1_marshal(a, BURROW_ANY(TYPE_OF(Tagged), &x), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Marshal failed: %s", err_str(err));
    Tagged y;
    memset(&y, 0, sizeof y);
    asn1_unmarshal(a, enc, BURROW_ANY(TYPE_OF(Tagged), &y), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Unmarshal failed: %s", err_str(err));
    if (!deep_equal(TYPE_OF(Tagged), &x, &y))
        testing_t_fatal_v(t, "Unexpected diff after roundtripping struct");
    arena_free(&ar);
}

static void TestParsingMemoryConsumption(TestingT *t) {
    /* Craft a syntactically valid, but empty, ~10 MB DER bomb. A successful
     * unmarshal of this bomb should yield ~280 MB. However, the parsing should
     * fail due to the empty content; and, in such cases, we want to make sure
     * that we do not unnecessarily allocate memories. */
    Int n = 10000000;
    Slice bomb = slice_make(heap_allocator(), TYPE_BYTE, n + 5, n + 5);
    Byte *p = (Byte *)bomb.p;
    memcpy(p, "\x30\x83\x98\x96\x80", 5);
    memset(p + 5, 0x30, (size_t)n);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint64_t before = mem_stats(a).bytes_total;

    BombElemSlice out = slice_nil(TYPE_OF(BombElem));
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(a, bomb, BURROW_ANY(TYPE_OF(BombElemSlice), &out), &err);
    if (errors_as(err, TYPE_ASN1_SYNTAX_ERROR) == NULL)
        testing_t_fatalf_v(
            t, "Incorrect error result: want a SyntaxError, but got (%s) instead",
            err_str(err));

    uint64_t diff = mem_stats(a).bytes_total - before;
    /* Ensure that the memory allocated does not exceed 10<<21 (~20 MB) when
     * the parsing fails. */
    if (diff > (uint64_t)10 << 21)
        testing_t_errorf_v(t, "Too much memory allocated while parsing DER: %d MiB",
                           (int64_t)(diff / 1024 / 1024));
    arena_free(&ar);
    mem_free(heap_allocator(), bomb.p, (size_t)bomb.cap, 1);
}

/* The nesting tests marshal and unmarshal ten thousand levels, which takes
 * more stack than a test goroutine has, so they run on one with 256 MiB. */
typedef struct DeepJob {
    void (*run)(struct DeepJob *j);
    Int limit;
    Error below;
    Error above;
    Error make;
    Chan *done;
} DeepJob;

static void deep_entry(void *env) {
    DeepJob *j = env;
    j->run(j);
    bool done = true;
    chan_send(j->done, &done);
}

static void run_deep(TestingT *t, DeepJob *j) {
    j->done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (!go_stack(BURROW_FN(Func, deep_entry, j), (size_t)256 << 20))
        testing_t_fatalf_v(t, "go_stack failed");
    bool done;
    chan_recv(j->done, &done);
    chan_free(j->done);
}

static void check_deep(TestingT *t, const DeepJob *j) {
    if (BURROW_FAILED(j->make))
        testing_t_fatalf_v(t, "Marshal failed: %s", err_str(j->make));
    if (BURROW_FAILED(j->below))
        testing_t_errorf_v(t, "below limit: Unmarshal failed at depth %d: %s", j->limit,
                           err_str(j->below));
    if (BURROW_OK(j->above))
        testing_t_fatalf_v(t,
                           "above limit: Unmarshal succeeded at depth %d, want error",
                           j->limit + 1);
    Str want = BURROW_S("asn1: structure error: nesting depth exceeded");
    if (!str_eq(error_text(j->above), want))
        testing_t_errorf_v(t,
                           "above limit: Unmarshal error mismatch\ngot:  %q\nwant: %q",
                           error_text(j->above), want);
}

static Slice recursive_data(Alloc *a, Int depth, Error *err) {
    Recursive *levels =
        mem_alloc(a, (size_t)depth * sizeof(Recursive), _Alignof(Recursive));
    levels[0] = slice_nil(TYPE_OF(Recursive));
    for (Int i = 1; i < depth; i++)
        levels[i] = (Recursive){&levels[i - 1], 1, 1, TYPE_OF(Recursive)};
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(Recursive), &levels[depth - 1]), err);
}

static void deep_slice(DeepJob *j) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = recursive_data(a, j->limit, &j->make);
    Recursive r = slice_nil(TYPE_OF(Recursive));
    if (BURROW_OK(j->make))
        asn1_unmarshal(a, data, BURROW_ANY(TYPE_OF(Recursive), &r), &j->below);
    data = recursive_data(a, j->limit + 1, &j->make);
    r = slice_nil(TYPE_OF(Recursive));
    if (BURROW_OK(j->make))
        asn1_unmarshal(a, data, BURROW_ANY(TYPE_OF(Recursive), &r), &j->above);
    /* The errors are in the error arena, so the data can go. */
    arena_free(&ar);
}

static void TestUnmarshalNestingLimitSlice(TestingT *t) {
    DeepJob j = {deep_slice,      10000,           BURROW_NO_ERROR,
                 BURROW_NO_ERROR, BURROW_NO_ERROR, NULL};
    run_deep(t, &j);
    check_deep(t, &j);
}

static Slice recursive_struct_data(Alloc *a, Int depth, Error *err) {
    RecursiveStruct *levels = mem_alloc(a, (size_t)depth * sizeof(RecursiveStruct),
                                        _Alignof(RecursiveStruct));
    levels[0].Next = slice_nil(TYPE_OF(RecursiveStruct));
    for (Int i = 1; i < depth; i++)
        levels[i].Next =
            (RecursiveStructSlice){&levels[i - 1], 1, 1, TYPE_OF(RecursiveStruct)};
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(RecursiveStruct), &levels[depth - 1]),
                        err);
}

static void deep_struct(DeepJob *j) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = recursive_struct_data(a, j->limit, &j->make);
    RecursiveStruct r;
    memset(&r, 0, sizeof r);
    if (BURROW_OK(j->make))
        asn1_unmarshal(a, data, BURROW_ANY(TYPE_OF(RecursiveStruct), &r), &j->below);
    data = recursive_struct_data(a, j->limit + 1, &j->make);
    memset(&r, 0, sizeof r);
    if (BURROW_OK(j->make))
        asn1_unmarshal(a, data, BURROW_ANY(TYPE_OF(RecursiveStruct), &r), &j->above);
    arena_free(&ar);
}

/* Note that recursive structs fail in half the normal limit because each
 * level of nesting in a struct (with a slice field) involves two depth
 * increments (one for the struct and one for the slice). */
static void TestUnmarshalNestingLimitStruct(TestingT *t) {
    DeepJob j = {deep_struct,     5000, BURROW_NO_ERROR, BURROW_NO_ERROR,
                 BURROW_NO_ERROR, NULL};
    run_deep(t, &j);
    check_deep(t, &j);
}

/* ---------------------------------------------------------------- marshal */

/* The table of TestMarshal, which BenchmarkMarshal runs too when b is not
 * NULL. */
static void marshal_table(TestingT *t, TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TimeLocation *pst = time_fixed_zone(a, BURROW_S("PST"), -8 * 60 * 60);
    Error perr = BURROW_NO_ERROR;
    Time far_future =
        time_parse(a, TIME_RFC3339, BURROW_S("2100-04-05T12:01:01Z"), &perr);
    if (BURROW_FAILED(perr))
        testing_t_fatal_v(t, err_str(perr));

    Byte xs[128];
    memset(xs, 'x', sizeof xs);
    Str x127 = {xs, 127};
    Str x128 = {xs, 128};
    char x127_hex[2 * 127 + 5];
    char x128_hex[2 * 128 + 7];
    memcpy(x127_hex, "137f", 4);
    memcpy(x128_hex, "138180", 6);
    for (int i = 0; i < 127; i++)
        memcpy(x127_hex + 4 + 2 * i, "78", 2);
    for (int i = 0; i < 128; i++)
        memcpy(x128_hex + 6 + 2 * i, "78", 2);
    x127_hex[4 + 2 * 127] = '\0';
    x128_hex[6 + 2 * 128] = '\0';

    const struct {
        Any in;
        const char *out;
    } tests[] = {
        {BURROW_ANY(TYPE_INT, &(Int){10}), "02010a"},
        {BURROW_ANY(TYPE_INT, &(Int){127}), "02017f"},
        {BURROW_ANY(TYPE_INT, &(Int){128}), "02020080"},
        {BURROW_ANY(TYPE_INT, &(Int){-128}), "020180"},
        {BURROW_ANY(TYPE_INT, &(Int){-129}), "0202ff7f"},
        {BURROW_ANY(TYPE_OF(IntStruct), (&(IntStruct){64})), "3003020140"},
        {BURROW_ANY(TYPE_OF(BigIntStruct), (&(BigIntStruct){big_new_int(a, 0x123456)})),
         "30050203123456"},
        {BURROW_ANY(TYPE_OF(TwoIntStruct), (&(TwoIntStruct){64, 65})),
         "3006020140020141"},
        {BURROW_ANY(TYPE_OF(NestedStruct), (&(NestedStruct){{127}})), "3005300302017f"},
        {BURROW_ANY(TYPE_BYTES, &BYTES(1, 2, 3)), "0403010203"},
        {BURROW_ANY(TYPE_OF(ImplicitTagTest), (&(ImplicitTagTest){64})), "3003850140"},
        {BURROW_ANY(TYPE_OF(ExplicitTagTest), (&(ExplicitTagTest){64})),
         "3005a503020140"},
        {BURROW_ANY(TYPE_OF(FlagTest), (&(FlagTest){true})), "30028000"},
        {BURROW_ANY(TYPE_OF(FlagTest), (&(FlagTest){false})), "3000"},
        {BURROW_ANY(TYPE_TIME, (Time[]){time_utc(time_from_unix(0, 0))}),
         "170d3730303130313030303030305a"},
        {BURROW_ANY(TYPE_TIME, (Time[]){time_utc(time_from_unix(1258325776, 0))}),
         "170d3039313131353232353631365a"},
        {BURROW_ANY(TYPE_TIME, (Time[]){time_in(time_from_unix(1258325776, 0), pst)}),
         "17113039313131353134353631362d30383030"},
        {BURROW_ANY(TYPE_TIME, &far_future), "180f32313030303430353132303130315a"},
        {BURROW_ANY(TYPE_OF(GeneralizedTimeTest),
                    (&(GeneralizedTimeTest){time_utc(time_from_unix(1258325776, 0))})),
         "3011180f32303039313131353232353631365a"},
        {BURROW_ANY(TYPE_ASN1_BIT_STRING, (&(Asn1BitString){BYTES(0x80), 1})),
         "03020780"},
        {BURROW_ANY(TYPE_ASN1_BIT_STRING, (&(Asn1BitString){BYTES(0x81, 0xf0), 12})),
         "03030481f0"},
        {BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &ASN1_OID(1, 2, 3, 4)), "06032a0304"},
        {BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &ASN1_OID(1, 2, 840, 133549, 1, 1, 5)),
         "06092a864888932d010105"},
        {BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &ASN1_OID(2, 100, 3)), "0603813403"},
        {BURROW_ANY(TYPE_STRING, (Str[]){BURROW_S("test")}), "130474657374"},
        {BURROW_ANY(TYPE_STRING, &x127), x127_hex},
        {BURROW_ANY(TYPE_STRING, &x128), x128_hex},
        {BURROW_ANY(TYPE_OF(Ia5StringTest), (&(Ia5StringTest){BURROW_S("test")})),
         "3006160474657374"},
        {BURROW_ANY(TYPE_OF(OptionalRawValueTest), (&(OptionalRawValueTest){{0}})),
         "3000"},
        {BURROW_ANY(TYPE_OF(PrintableStringTest),
                    (&(PrintableStringTest){BURROW_S("test")})),
         "3006130474657374"},
        {BURROW_ANY(TYPE_OF(PrintableStringTest),
                    (&(PrintableStringTest){BURROW_S("test*")})),
         "30071305746573742a"},
        {BURROW_ANY(TYPE_OF(GenericStringTest),
                    (&(GenericStringTest){BURROW_S("test")})),
         "3006130474657374"},
        {BURROW_ANY(TYPE_OF(GenericStringTest),
                    (&(GenericStringTest){BURROW_S("test*")})),
         "30070c05746573742a"},
        {BURROW_ANY(TYPE_OF(GenericStringTest),
                    (&(GenericStringTest){BURROW_S("test&")})),
         "30070c057465737426"},
        {BURROW_ANY(TYPE_OF(RawContentsStruct),
                    (&(RawContentsStruct){slice_nil(TYPE_BYTE), 64})),
         "3003020140"},
        {BURROW_ANY(TYPE_OF(RawContentsStruct),
                    (&(RawContentsStruct){BYTES(0x30, 3, 1, 2, 3), 64})),
         "3003010203"},
        {BURROW_ANY(TYPE_ASN1_RAW_VALUE,
                    (&(Asn1RawValue){.tag = 1, .cls = 2, .bytes = BYTES(1, 2, 3)})),
         "8103010203"},
        {BURROW_ANY(TYPE_OF(testSET), (&(testSET){(Int[]){10}, 1, 1, TYPE_INT})),
         "310302010a"},
        {BURROW_ANY(TYPE_OF(OmitEmptyTest),
                    (&(OmitEmptyTest){slice_make(a, TYPE_STRING, 0, 0)})),
         "3000"},
        {BURROW_ANY(TYPE_OF(OmitEmptyTest),
                    (&(OmitEmptyTest){{(Str[]){BURROW_S("1")}, 1, 1, TYPE_STRING}})),
         "30053003130131"},
        {BURROW_ANY(TYPE_STRING, (Str[]){BURROW_S("\xce\xa3")}), "0c02cea3"},
        {BURROW_ANY(TYPE_OF(DefaultTest), (&(DefaultTest){0})), "3003020100"},
        {BURROW_ANY(TYPE_OF(DefaultTest), (&(DefaultTest){1})), "3000"},
        {BURROW_ANY(TYPE_OF(DefaultTest), (&(DefaultTest){2})), "3003020102"},
        {BURROW_ANY(TYPE_OF(ApplicationTest), (&(ApplicationTest){1, 2})),
         "30084001016103020102"},
        {BURROW_ANY(TYPE_OF(PrivateTest), (&(PrivateTest){1, 2, 3, 4})),
         "3011c00101e103020102df1f0103df81000104"},
        {BURROW_ANY(TYPE_OF(NumericStringTest),
                    (&(NumericStringTest){BURROW_S("1 9")})),
         "30051203312039"},
    };
    if (b != NULL) {
        ArenaMark m = arena_mark(&ar);
        testing_b_report_allocs(b);
        testing_b_reset_timer(b);
        for (Int n = 0; n < testing_b_n(b); n++) {
            for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
                Error err = BURROW_NO_ERROR;
                asn1_marshal(a, tests[i].in, &err);
            }
            arena_release(&ar, m);
        }
        arena_free(&ar);
        return;
    }
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Slice data = asn1_marshal(a, tests[i].in, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d failed: %s", (int64_t)i, err_str(err));
        Slice out = unhex(a, tests[i].out);
        if (!same_bytes(out, data))
            testing_t_errorf_v(t, "#%d got: %s want %s", (int64_t)i, tohex(a, data),
                               tohex(a, out));
    }
    arena_free(&ar);
}

static void TestMarshal(TestingT *t) {
    marshal_table(t, NULL);
}

static void BenchmarkMarshal(TestingB *b) {
    marshal_table(NULL, b);
}

static void TestMarshalWithParams(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    IntStruct ten = {10};
    const struct {
        const char *params;
        const char *out;
    } tests[] = {
        {"set", "310302010a"},
        {"application", "600302010a"},
        {"private", "e00302010a"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Slice data = asn1_marshal_with_params(a, BURROW_ANY(TYPE_OF(IntStruct), &ten),
                                              cstr(tests[i].params), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d failed: %s", (int64_t)i, err_str(err));
        Slice out = unhex(a, tests[i].out);
        if (!same_bytes(out, data))
            testing_t_errorf_v(t, "#%d got: %s want %s", (int64_t)i, tohex(a, data),
                               tohex(a, out));
    }
    arena_free(&ar);
}

static void TestMarshalError(TestingT *t) {
    const struct {
        Any in;
        const char *err;
    } tests[] = {
        {BURROW_ANY(TYPE_OF(BigIntStruct), (&(BigIntStruct){NULL})), "empty integer"},
        {BURROW_ANY(TYPE_OF(NumericStringTest), (&(NumericStringTest){BURROW_S("a")})),
         "invalid character"},
        {BURROW_ANY(TYPE_OF(Ia5StringTest), (&(Ia5StringTest){BURROW_S("\xb0")})),
         "invalid character"},
        {BURROW_ANY(TYPE_OF(PrintableStringTest),
                    (&(PrintableStringTest){BURROW_S("!")})),
         "invalid character"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        asn1_marshal(heap_allocator(), tests[i].in, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "#%d should fail, but success", (int64_t)i);
            continue;
        }
        if (!strings_contains(error_text(err), cstr(tests[i].err)))
            testing_t_errorf_v(t, "#%d got: %s want %s", (int64_t)i, error_text(err),
                               cstr(tests[i].err));
    }
}

static void TestInvalidUTF8(TestingT *t) {
    Str s = {(const Byte *)"\xff\xff", 2};
    Error err = BURROW_NO_ERROR;
    asn1_marshal(heap_allocator(), BURROW_ANY(TYPE_STRING, &s), &err);
    if (BURROW_OK(err))
        testing_t_error_v(t, "invalid UTF8 string was accepted");
}

static void TestMarshalOID(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        Any in;
        const char *out;
    } tests[] = {
        /* The bytes format returns a byte sequence \x04. */
        {BURROW_ANY(TYPE_BYTES, (Slice[]){LIT("\x06\x01\x30")}), "0403060130"},
        /* {ObjectIdentifier([]int{0}), "060100"}, returns an error as OID
         * 0.0 has the same encoding. */
        /* The same as above: "\x06\x010" is "\x06\x01" and "0". */
        {BURROW_ANY(TYPE_BYTES, (Slice[]){LIT("\x06\x01"
                                              "0")}),
         "0403060130"},
        /* The example in ITU-T X.690. */
        {BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &ASN1_OID(2, 999, 3)), "0603883703"},
        /* The zero OID. */
        {BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &ASN1_OID(0, 0)), "060100"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Slice data = asn1_marshal(a, tests[i].in, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d failed: %s", (int64_t)i, err_str(err));
        Slice out = unhex(a, tests[i].out);
        if (!same_bytes(out, data))
            testing_t_errorf_v(t, "#%d got: %s want %s", (int64_t)i, tohex(a, data),
                               tohex(a, out));
    }
    arena_free(&ar);
}

static void TestIssue11130(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice data = LIT("\x06\x01"
                     "0"); /* == \x06\x01\x30 == OID = 0 (the figure) */
    Any v = {NULL, NULL};
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(a, data, BURROW_ANY(TYPE_ANY, &v), &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err_str(err));
        goto done;
    }
    if (v.t != TYPE_ASN1_OBJECT_IDENTIFIER) {
        testing_t_error_v(t, "marshal OID returned an invalid type");
        goto done;
    }
    Slice data1 = asn1_marshal(a, v, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err_str(err));
        goto done;
    }
    if (!same_bytes(data, data1)) {
        testing_t_errorf_v(t, "got: %s, want: %s", tohex(a, data1), tohex(a, data));
        goto done;
    }
    Any v1 = {NULL, NULL};
    asn1_unmarshal(a, data1, BURROW_ANY(TYPE_ANY, &v1), &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err_str(err));
        goto done;
    }
    if (!deep_equal(TYPE_ANY, &v, &v1))
        testing_t_errorf_v(t, "got and want differ, data=%s", tohex(a, data1));
done:
    arena_free(&ar);
}

static void TestIssue68241(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    bool wants[] = {false, true};
    for (size_t i = 0; i < 2; i++) {
        Any want = BURROW_ANY(TYPE_BOOL, &wants[i]);
        Error err = BURROW_NO_ERROR;
        Slice data = asn1_marshal(a, want, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "cannot Marshal: %s", err_str(err));
            break;
        }
        Any got = {NULL, NULL};
        asn1_unmarshal(a, data, BURROW_ANY(TYPE_ANY, &got), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "cannot Unmarshal: %s", err_str(err));
            break;
        }
        if (!deep_equal(TYPE_ANY, &got, &want))
            testing_t_errorf_v(t, "#%d Unmarshal, got and want differ", (int64_t)i);
    }
    arena_free(&ar);
}

static const char *const set_input[] = {"a", "aa", "b", "bb", "c", "cc"};
static const char *const set_order[] = {"a", "b", "c", "aa", "bb", "cc"};

static StrSlice set_strings(Alloc *a, const char *const *s) {
    StrSlice out = slice_make(a, TYPE_STRING, 6, 6);
    for (Int i = 0; i < 6; i++)
        ((Str *)out.p)[i] = cstr(s[i]);
    return out;
}

static void TestSetEncoder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SetStrings test_struct = {set_strings(a, set_input)};

    /* The expected ordering of the SET is a, b, c, aa, bb, cc. */
    Error err = BURROW_NO_ERROR;
    Slice output = asn1_marshal(a, BURROW_ANY(TYPE_OF(SetStrings), &test_struct), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err_str(err));

    StrSlice expected = set_strings(a, set_order);
    SetStrings result = {slice_nil(TYPE_STRING)};
    Slice rest =
        asn1_unmarshal(a, output, BURROW_ANY(TYPE_OF(SetStrings), &result), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err_str(err));
    if (rest.len != 0)
        testing_t_error_v(t, "Unmarshal returned extra garbage");
    if (!deep_equal(TYPE_OF(StrSlice), &expected, &result.Strings))
        testing_t_errorf_v(t, "Unexpected SET content: %s", tohex(a, output));
    arena_free(&ar);
}

static void TestSetEncoderSETSliceSuffix(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testSetSET test_set = set_strings(a, set_input);

    /* The expected ordering of the SET is a, b, c, aa, bb, cc. */
    Error err = BURROW_NO_ERROR;
    Slice output = asn1_marshal(a, BURROW_ANY(TYPE_OF(testSetSET), &test_set), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err_str(err));

    testSetSET expected = set_strings(a, set_order);
    testSetSET result = slice_nil(TYPE_STRING);
    Slice rest =
        asn1_unmarshal(a, output, BURROW_ANY(TYPE_OF(testSetSET), &result), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err_str(err));
    if (rest.len != 0)
        testing_t_error_v(t, "Unmarshal returned extra garbage");
    if (!deep_equal(TYPE_OF(testSetSET), &expected, &result))
        testing_t_errorf_v(t, "Unexpected SET content: %s", tohex(a, output));
    arena_free(&ar);
}

static void BenchmarkObjectIdentifierString(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Asn1ObjectIdentifier oid_public_key_rsa = ASN1_OID(1, 2, 840, 113549, 1, 1, 1);
    ArenaMark m = arena_mark(&ar);
    for (Int i = 0; i < testing_b_n(b); i++) {
        asn1_object_identifier_string(oid_public_key_rsa, a);
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------ no memory */

static void TestNoMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *scratch = arena_allocator(&ar);
    Certificate want = self_signed_cert(scratch);
    Slice der =
        lit((const char *)der_self_signed_cert, (Int)sizeof der_self_signed_cert);

    /* Unmarshal has nothing to give back on a failure, since what it built
     * belongs to the value, so it runs on an arena that is dropped each
     * time. */
    for (long long budget = 0;; budget++) {
        Arena inner;
        arena_init(&inner, NULL, 0);
        Budget b = {arena_allocator(&inner), budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        Certificate cert;
        memset(&cert, 0, sizeof cert);
        Error err = BURROW_NO_ERROR;
        asn1_unmarshal(&al, der, BURROW_ANY(TYPE_OF(Certificate), &cert), &err);
        bool done = BURROW_OK(err);
        if (done && !deep_equal(TYPE_OF(Certificate), &cert, &want))
            testing_t_errorf_v(t, "budget %d: the certificate came out wrong",
                               (int64_t)budget);
        arena_free(&inner);
        if (done)
            break;
        if (budget > 100000)
            testing_t_fatal_v(t, "unmarshaling never finished");
    }

    /* Marshal gives back everything but its result. */
    for (long long budget = 0;; budget++) {
        Budget b = {heap_allocator(), budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        Error err = BURROW_NO_ERROR;
        Slice out = asn1_marshal(&al, BURROW_ANY(TYPE_OF(Certificate), &want), &err);
        if (BURROW_FAILED(err)) {
            if (b.live != 0)
                testing_t_errorf_v(t, "budget %d: %d bytes leaked marshaling",
                                   (int64_t)budget, (int64_t)b.live);
            if (budget > 100000)
                testing_t_fatal_v(t, "marshaling never finished");
            continue;
        }
        if (b.live != (long long)out.cap)
            testing_t_errorf_v(t, "budget %d: %d bytes live, want the %d of the result",
                               (int64_t)budget, (int64_t)b.live, out.cap);
        Certificate back;
        memset(&back, 0, sizeof back);
        asn1_unmarshal(scratch, out, BURROW_ANY(TYPE_OF(Certificate), &back), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "reading back what was marshaled: %s", err_str(err));
        mem_free(&al, out.p, (size_t)out.cap, 1);
        break;
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestParseBool)                                                                   \
    X(TestParseInt64)                                                                  \
    X(TestParseInt32)                                                                  \
    X(TestParseBigInt)                                                                 \
    X(TestBitString)                                                                   \
    X(TestBitStringAt)                                                                 \
    X(TestBitStringRightAlign)                                                         \
    X(TestObjectIdentifier)                                                            \
    X(TestUTCTime)                                                                     \
    X(TestGeneralizedTime)                                                             \
    X(TestParseTagAndLength)                                                           \
    X(TestParseFieldParameters)                                                        \
    X(TestUnmarshal)                                                                   \
    X(TestUnmarshalWithNilOrNonPointer)                                                \
    X(TestCertificate)                                                                 \
    X(TestCertificateWithNUL)                                                          \
    X(TestRawStructs)                                                                  \
    X(TestObjectIdentifierEqual)                                                       \
    X(TestStringSlice)                                                                 \
    X(TestExplicitTaggedTime)                                                          \
    X(TestImplicitTaggedTime)                                                          \
    X(TestTruncatedExplicitTag)                                                        \
    X(TestUnmarshalInvalidUTF8)                                                        \
    X(TestMarshalNilValue)                                                             \
    X(TestUnexportedStructField)                                                       \
    X(TestNull)                                                                        \
    X(TestExplicitTagRawValueStruct)                                                   \
    X(TestTaggedRawValue)                                                              \
    X(TestBMPString)                                                                   \
    X(TestNonMinimalEncodedOID)                                                        \
    X(TestImplicitTypeRoundtrip)                                                       \
    X(TestParsingMemoryConsumption)                                                    \
    X(TestUnmarshalNestingLimitSlice)                                                  \
    X(TestUnmarshalNestingLimitStruct)                                                 \
    X(TestMarshal)                                                                     \
    X(TestMarshalWithParams)                                                           \
    X(TestMarshalError)                                                                \
    X(TestInvalidUTF8)                                                                 \
    X(TestMarshalOID)                                                                  \
    X(TestIssue11130)                                                                  \
    X(TestIssue68241)                                                                  \
    X(TestSetEncoder)                                                                  \
    X(TestSetEncoderSETSliceSuffix)                                                    \
    X(TestNoMemory)                                                                    \
    X(BenchmarkMarshal)                                                                \
    X(BenchmarkUnmarshal)                                                              \
    X(BenchmarkObjectIdentifierString)

TESTING_MAIN(TESTS)
