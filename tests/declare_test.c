/* Tests for the declaration DSL.
 *
 * The thing being tested is that the struct and its descriptor cannot disagree,
 * because they come out of one list. So most of what is below compares the
 * descriptor against what the compiler thinks, using offsetof and sizeof on the
 * very struct the macro emitted. A test that hardcoded the offsets would be
 * testing this machine's ABI rather than the DSL, and would go red on the first
 * platform with different padding rather than on the first real bug.
 *
 * The other half is the part that is easy to get wrong and impossible to see: a
 * struct is not its bytes. It has padding in it, nothing says what is in the
 * padding, and two values that Go calls equal routinely differ there. Anything
 * that compares or hashes a struct has to skip it, and both have to skip the
 * same thing or a map holds two entries for one key.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/declare.h"

#include "burrow/core.h"
#include "burrow/map.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "check.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------- the example
 *
 * The one from the header, so that the documentation is a thing that runs. */

#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "json:\"x\"")                                                         \
    F(T, Int, Y, "json:\"y\"")                                                         \
    F(T, Str, Label, "json:\"label,omitempty\"")

BURROW_STRUCT(Point, POINT_FIELDS);

static void TestADeclaredStructIsTheStructYouWrote(TestingT *t) {
    Point p = {3, 4, BURROW_S("origin")};

    CHECK_INT_EQ((int)p.X, 3);
    CHECK_INT_EQ((int)p.Y, 4);
    CHECK(str_eq(p.Label, BURROW_S("origin")));
}

static void TestADeclaredStructDescribesItself(TestingT *t) {
    const Type *ty = TYPE_OF(Point);

    CHECK(ty != NULL);
    CHECK(str_eq(ty->name, BURROW_S("Point")));
    CHECK(ty->kind == KIND_STRUCT);
    CHECK_INT_EQ((int)ty->size, (int)sizeof(Point));
    CHECK_INT_EQ((int)ty->align, (int)_Alignof(Point));
    CHECK_INT_EQ((int)ty->nfield, 3);
}

static void TestTheOffsetsAreTheCompilersOffsets(TestingT *t) {
    const Type *ty = TYPE_OF(Point);

    /* The whole point of the DSL in one check. These are not compared against
     * numbers, they are compared against what the compiler laid out, so this
     * says the same thing on a machine that pads differently. */
    CHECK_INT_EQ((int)ty->fields[0].offset, (int)offsetof(Point, X));
    CHECK_INT_EQ((int)ty->fields[1].offset, (int)offsetof(Point, Y));
    CHECK_INT_EQ((int)ty->fields[2].offset, (int)offsetof(Point, Label));

    CHECK(str_eq(ty->fields[0].name, BURROW_S("X")));
    CHECK(str_eq(ty->fields[2].name, BURROW_S("Label")));

    CHECK(ty->fields[0].type == TYPE_INT);
    CHECK(ty->fields[2].type == TYPE_STRING);
}

static void TestTheTagsComeThroughExactlyAsWritten(TestingT *t) {
    const Type *ty = TYPE_OF(Point);

    /* Unparsed, because encoding/json and encoding/xml and database/sql each
     * read it their own way and Go does not centralise that either. */
    CHECK(str_eq(ty->fields[0].tag, BURROW_S("json:\"x\"")));
    CHECK(str_eq(ty->fields[2].tag, BURROW_S("json:\"label,omitempty\"")));
}

static void TestAFieldIsFoundByName(TestingT *t) {
    const Field *f = type_field_by_name(TYPE_OF(Point), BURROW_S("Label"));

    CHECK(f != NULL);
    if (f != NULL) {
        CHECK_INT_EQ((int)f->offset, (int)offsetof(Point, Label));
        CHECK(f->type == TYPE_STRING);
    }

    CHECK(type_field_by_name(TYPE_OF(Point), BURROW_S("Nope")) == NULL);
}

/* ------------------------------------------------------------------ padding
 *
 * A struct with a hole in it. char then int is three bytes of nothing on every
 * machine with four byte alignment, and the descriptor has to know that those
 * three bytes are not part of the value.
 *
 * Both values below are built by hand out of bytes rather than by assignment,
 * because an assignment is free to copy the padding along with the fields and
 * then the two would agree by luck. */

#define GAPPY_FIELDS(F, T)                                                             \
    F(T, uint8_t, Small, "")                                                           \
    F(T, int32_t, Big, "")

BURROW_STRUCT(Gappy, GAPPY_FIELDS);

/* Write a byte into every part of a value that no field covers.
 *
 * Driven off the descriptor rather than off the field names, so it says the
 * same thing wherever the padding lands, and so the test would still be testing
 * something if a future change moved the hole. */
static void fill_padding(const Type *t, void *value, Byte with) {
    Byte *b = (Byte *)value;

    for (uint32_t i = 0; i < t->size; i++) {
        bool covered = false;
        for (uint16_t f = 0; f < t->nfield && !covered; f++)
            covered = i >= t->fields[f].offset &&
                      i < t->fields[f].offset + t->fields[f].type->size;
        if (!covered)
            b[i] = with;
    }
}

static void TestTwoStructsThatDifferOnlyInTheirPaddingAreEqual(TestingT *t) {
    Gappy a;
    Gappy b;

    memset(&a, 0x00, sizeof a);
    memset(&b, 0xff, sizeof b);

    a.Small = 7;
    a.Big = -9;
    b.Small = 7;
    b.Big = -9;

    fill_padding(TYPE_OF(Gappy), &a, 0x00);
    fill_padding(TYPE_OF(Gappy), &b, 0xff);

    /* memcmp says these differ, and it is wrong, which is the entire reason the
     * descriptor walks fields instead. */
    CHECK(memcmp(&a, &b, sizeof a) != 0 ||
          sizeof(Gappy) == sizeof(uint8_t) + sizeof(int32_t));
    CHECK(type_equal(TYPE_OF(Gappy), &a, &b));
}

static void TestTwoStructsThatCompareEqualHashTheSame(TestingT *t) {
    Gappy a;
    Gappy b;

    memset(&a, 0x00, sizeof a);
    memset(&b, 0xff, sizeof b);

    a.Small = 7;
    a.Big = -9;
    b.Small = 7;
    b.Big = -9;

    fill_padding(TYPE_OF(Gappy), &a, 0x00);
    fill_padding(TYPE_OF(Gappy), &b, 0xff);

    /* The half that is easy to forget. Equality that skips the padding and a
     * hash that does not is worse than getting both wrong: a map would hold two
     * entries for one key and return whichever it probed first. */
    CHECK(type_hash(TYPE_OF(Gappy), &a, 0) == type_hash(TYPE_OF(Gappy), &b, 0));
}

static void TestDifferentValuesStillDiffer(TestingT *t) {
    Gappy a = {1, 2};
    Gappy b = {1, 3};

    CHECK(!type_equal(TYPE_OF(Gappy), &a, &b));
    CHECK(type_hash(TYPE_OF(Gappy), &a, 0) != type_hash(TYPE_OF(Gappy), &b, 0));
}

/* ------------------------------------------------------------ a Str inside
 *
 * The other thing the field walk buys. A Str is a pointer and a length, and two
 * Str values with different pointers and the same bytes are one string in Go.
 * Comparing the enclosing struct as bytes would compare the pointers. */

static void TestAStringFieldComparesByItsBytes(TestingT *t) {
    char first[] = "origin";
    char second[] = "origin";
    Point a = {1, 2, {(const Byte *)first, 6}};
    Point b = {1, 2, {(const Byte *)second, 6}};

    CHECK(a.Label.p != b.Label.p);
    CHECK(type_equal(TYPE_OF(Point), &a, &b));
    CHECK(type_hash(TYPE_OF(Point), &a, 0) == type_hash(TYPE_OF(Point), &b, 0));
}

static void TestADeclaredStructWorksAsAMapKey(TestingT *t) {
    /* The payoff for all of the above, and the place a padding bug would show
     * up as a lookup that misses for no visible reason. */
    Map *m = map_make(heap_allocator(), TYPE_OF(Point), TYPE_INT, 0);
    CHECK(m != NULL);
    if (m == NULL)
        return;

    char label[] = "a";
    Point key = {1, 2, {(const Byte *)label, 1}};
    Int value = 42;
    map_set(m, &key, &value);

    /* A second key with the same contents and nothing else in common: built
     * separately, with its own string. */
    char other[] = "a";
    Point same = {1, 2, {(const Byte *)other, 1}};
    Int got = 0;
    CHECK(map_get2(m, &same, &got));
    CHECK_INT_EQ((int)got, 42);
    CHECK_INT_EQ((int)map_len(m), 1);

    /* And setting through the second one does not add an entry. */
    Int replacement = 43;
    map_set(m, &same, &replacement);
    CHECK_INT_EQ((int)map_len(m), 1);

    map_free(m);
}

/* ------------------------------------------------------------------ nesting
 *
 * A struct with a struct in it, which is where a descriptor that only knew
 * about builtins would stop being useful. */

#define LINE_FIELDS(F, T)                                                              \
    F(T, Point, From, "json:\"from\"")                                                 \
    F(T, Point, To, "json:\"to\"")

BURROW_STRUCT(Line, LINE_FIELDS);

static void TestAStructFieldCarriesItsOwnDescriptor(TestingT *t) {
    const Type *ty = TYPE_OF(Line);

    CHECK_INT_EQ((int)ty->nfield, 2);
    CHECK(ty->fields[0].type == TYPE_OF(Point));
    CHECK_INT_EQ((int)ty->fields[1].offset, (int)offsetof(Line, To));
    CHECK_INT_EQ((int)ty->fields[0].type->nfield, 3);
}

static void TestEqualityRecursesIntoANestedStruct(TestingT *t) {
    char one[] = "a";
    char two[] = "a";
    Line a = {{1, 2, {(const Byte *)one, 1}}, {3, 4, BURROW_STR_EMPTY}};
    Line b = {{1, 2, {(const Byte *)two, 1}}, {3, 4, BURROW_STR_EMPTY}};

    CHECK(type_equal(TYPE_OF(Line), &a, &b));

    b.To.Y = 5;
    CHECK(!type_equal(TYPE_OF(Line), &a, &b));
}

/* --------------------------------------------------------- composite types
 *
 * Declared here rather than next to the tests for them, because the embedded
 * test below wants a slice type to prove a point with. */

BURROW_SLICE_TYPE(IntSlice, Int);
BURROW_PTR_TYPE(PointPtr, Point);
BURROW_ARRAY_TYPE(Int4, Int, 4);
BURROW_MAP_TYPE(StrIntMap, Str, Int);

/* ------------------------------------------------------- exported, embedded
 *
 * Neither is stored, both are questions about the name, and both decide what
 * encoding/json does with a field. */

#define MIXED_FIELDS(F, T)                                                             \
    F(T, Int, Visible, "")                                                             \
    F(T, Int, hidden, "")                                                              \
    F(T, Point, Point, "")

BURROW_STRUCT(Mixed, MIXED_FIELDS);

static void TestExportednessFollowsGosRule(TestingT *t) {
    const Type *ty = TYPE_OF(Mixed);

    CHECK(field_is_exported(&ty->fields[0]));
    CHECK(!field_is_exported(&ty->fields[1]));
    CHECK(!field_is_exported(NULL));
}

static void TestAnEmbeddedFieldIsOneNamedAfterItsType(TestingT *t) {
    const Type *ty = TYPE_OF(Mixed);

    CHECK(!field_is_embedded(&ty->fields[0]));
    CHECK(field_is_embedded(&ty->fields[2]));
    CHECK(!field_is_embedded(NULL));

    /* An unnamed type cannot be embedded, and the check must not fall back to
     * the kind name and call a field named "slice" embedded. */
    Field fake = {BURROW_S_INIT("slice"), BURROW_S_INIT(""), TYPE_OF(IntSlice), 0};
    CHECK(!field_is_embedded(&fake));
}

static void TestASliceDescriptorDescribesTheHeaderAndTheElement(TestingT *t) {
    const Type *ty = TYPE_OF(IntSlice);

    CHECK(ty->kind == KIND_SLICE);
    CHECK(ty->elem == TYPE_INT);
    /* The size of a slice value, which is the header. The elements live
     * somewhere else and there may be any number of them. */
    CHECK_INT_EQ((int)ty->size, (int)sizeof(Slice));
    CHECK(!type_is_comparable(ty));
}

static void TestAPointerDescriptorPointsAtSomething(TestingT *t) {
    const Type *ty = TYPE_OF(PointPtr);

    CHECK(ty->kind == KIND_POINTER);
    CHECK(ty->elem == TYPE_OF(Point));
    CHECK_INT_EQ((int)ty->size, (int)sizeof(void *));
    CHECK(type_is_comparable(ty));
}

static void TestAnArrayDescriptorIsItsElements(TestingT *t) {
    const Type *ty = TYPE_OF(Int4);

    CHECK(ty->kind == KIND_ARRAY);
    CHECK(ty->elem == TYPE_INT);
    CHECK_INT_EQ((int)ty->len, 4);
    /* Unlike a slice, an array is the elements, so it is four of them. */
    CHECK_INT_EQ((int)ty->size, (int)(4 * sizeof(Int)));

    Int4 a = {{1, 2, 3, 4}};
    Int4 b = {{1, 2, 3, 4}};
    CHECK(type_equal(ty, &a, &b));
    b.v[3] = 5;
    CHECK(!type_equal(ty, &a, &b));
}

static void TestAMapDescriptorCarriesBothHalves(TestingT *t) {
    const Type *ty = TYPE_OF(StrIntMap);

    CHECK(ty->kind == KIND_MAP);
    CHECK(ty->key == TYPE_STRING);
    CHECK(ty->elem == TYPE_INT);
    CHECK(!type_is_comparable(ty));
}

/* ------------------------------------------------------------ the builtins
 *
 * TYPE_OF is spelled in C and the TYPE_ names are spelled in Go, and the two
 * have to be the same descriptor or a field list and the rest of the library
 * would be describing different types. */

static void TestTheCSpellingAndTheGoSpellingAgree(TestingT *t) {
    CHECK(TYPE_OF(Int) == TYPE_INT);
    CHECK(TYPE_OF(Str) == TYPE_STRING);
    CHECK(TYPE_OF(bool) == TYPE_BOOL);
    CHECK(TYPE_OF(float) == TYPE_FLOAT32);
    CHECK(TYPE_OF(double) == TYPE_FLOAT64);
    CHECK(TYPE_OF(uint8_t) == TYPE_UINT8);

    /* byte is an alias of uint8 in Go and rune is an alias of int32, so these
     * are the same descriptor and not merely two that agree. */
    CHECK(TYPE_OF(Byte) == TYPE_UINT8);
    CHECK(TYPE_OF(Rune) == TYPE_INT32);
}

static void TestTheSameCTypeUnderTwoNamesStaysTwoTypes(TestingT *t) {
    /* Int is int64_t on a 64 bit machine, and Go's int and int64 are still
     * different types. The descriptor is keyed on the spelling for exactly this
     * reason: a struct with an int field does not marshal like one with an
     * int64 field. */
    CHECK(TYPE_OF(Int) != TYPE_OF(int64_t));
    CHECK(str_eq(TYPE_OF(Int)->name, BURROW_S("int")));
    CHECK(str_eq(TYPE_OF(int64_t)->name, BURROW_S("int64")));
}

/* ------------------------------------------------- a struct you did not write
 *
 * The case the boundary in burrow/type.h is really about. This struct is
 * somebody else's: it came out of a library header, it is a plain C struct, and
 * nobody is going to accept a patch that wraps it in a macro from a reflection
 * library. Describing it does not need one. BURROW_STRUCT_DECL emits the struct
 * and the declaration, BURROW_STRUCT_DEFINE emits only the descriptor, and the
 * second half works perfectly well against a struct that already exists.
 *
 * So there is a way out, it is one field list rather than a fork of the header,
 * and the test is here because a documented escape hatch that nobody compiles
 * is a documented escape hatch that stops working. */

struct vendor_rect {
    int32_t w;
    int32_t h;
    double area;
};

/* TYPE_OF pastes one identifier, and "struct vendor_rect" is two tokens. The
 * typedef is the whole adaptation. */
typedef struct vendor_rect VendorRect;

#define VENDOR_RECT_FIELDS(F, T)                                                       \
    F(T, int32_t, w, "json:\"width\"")                                                 \
    F(T, int32_t, h, "json:\"height\"")                                                \
    F(T, double, area, "json:\"-\"")

BURROW_STRUCT_DEFINE(VendorRect, VENDOR_RECT_FIELDS);

static void TestAStructNobodyDeclaredHereCanStillBeDescribed(TestingT *t) {
    const Type *ty = TYPE_OF(VendorRect);

    CHECK_INT_EQ(ty->kind, KIND_STRUCT);
    CHECK_INT_EQ(ty->nfield, 3);
    CHECK_INT_EQ(ty->size, (uint32_t)sizeof(VendorRect));
    CHECK_INT_EQ(ty->align, (uint16_t)_Alignof(VendorRect));

    /* The name is the typedef's, which is the name a lookup would use. */
    CHECK(str_eq(ty->name, BURROW_S("VendorRect")));

    CHECK_INT_EQ(ty->fields[0].offset, (uint32_t)offsetof(VendorRect, w));
    CHECK_INT_EQ(ty->fields[2].offset, (uint32_t)offsetof(VendorRect, area));
    CHECK(str_eq(ty->fields[0].tag, BURROW_S("json:\"width\"")));
}

static void TestADescribedForeignStructBehavesLikeADeclaredOne(TestingT *t) {
    const Type *ty = TYPE_OF(VendorRect);

    /* Built by hand, byte for byte, the way a vendor library would hand one
     * over. Padding deliberately left as it fell, which is the case equality
     * has to get right. */
    VendorRect a;
    VendorRect b;
    memset(&a, 0x5a, sizeof a);
    memset(&b, 0x00, sizeof b);
    a.w = b.w = 3;
    a.h = b.h = 4;
    a.area = b.area = 12.0;

    CHECK(type_equal(ty, &a, &b));
    CHECK_INT_EQ((int)(type_hash(ty, &a, 0) == type_hash(ty, &b, 0)), 1);

    b.h = 5;
    CHECK(!type_equal(ty, &a, &b));
}

#define TESTS(X)                                                                       \
    X(TestADeclaredStructIsTheStructYouWrote)                                          \
    X(TestADeclaredStructDescribesItself)                                              \
    X(TestTheOffsetsAreTheCompilersOffsets)                                            \
    X(TestTheTagsComeThroughExactlyAsWritten)                                          \
    X(TestAFieldIsFoundByName)                                                         \
    X(TestTwoStructsThatDifferOnlyInTheirPaddingAreEqual)                              \
    X(TestTwoStructsThatCompareEqualHashTheSame)                                       \
    X(TestDifferentValuesStillDiffer)                                                  \
    X(TestAStringFieldComparesByItsBytes)                                              \
    X(TestADeclaredStructWorksAsAMapKey)                                               \
    X(TestAStructFieldCarriesItsOwnDescriptor)                                         \
    X(TestEqualityRecursesIntoANestedStruct)                                           \
    X(TestExportednessFollowsGosRule)                                                  \
    X(TestAnEmbeddedFieldIsOneNamedAfterItsType)                                       \
    X(TestASliceDescriptorDescribesTheHeaderAndTheElement)                             \
    X(TestAPointerDescriptorPointsAtSomething)                                         \
    X(TestAnArrayDescriptorIsItsElements)                                              \
    X(TestAMapDescriptorCarriesBothHalves)                                             \
    X(TestTheCSpellingAndTheGoSpellingAgree)                                           \
    X(TestTheSameCTypeUnderTwoNamesStaysTwoTypes)                                      \
    X(TestAStructNobodyDeclaredHereCanStillBeDescribed)                                \
    X(TestADescribedForeignStructBehavesLikeADeclaredOne)

TESTING_MAIN(TESTS)
