/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/type.h"

#include "harness.h"

#include <stdlib.h>

static bool str_is(Str got, const char *want) {
    size_t n = strlen(want);
    if (got.len != (Int)n)
        return false;
    return n == 0 || memcmp(got.p, want, n) == 0;
}

/* Every kind has a name, and the name is the one Go's Kind.String prints. The
 * table is indexed by the enum, so a kind inserted in the middle without a name
 * inserted alongside it shows up here as a shifted answer rather than as a
 * crash, which is the only reason this walks all of them. */
TEST(every_kind_has_gos_name) {
    CHECK(str_is(kind_name(KIND_INVALID), "invalid"));
    CHECK(str_is(kind_name(KIND_BOOL), "bool"));
    CHECK(str_is(kind_name(KIND_INT), "int"));
    CHECK(str_is(kind_name(KIND_INT8), "int8"));
    CHECK(str_is(kind_name(KIND_INT16), "int16"));
    CHECK(str_is(kind_name(KIND_INT32), "int32"));
    CHECK(str_is(kind_name(KIND_INT64), "int64"));
    CHECK(str_is(kind_name(KIND_UINT), "uint"));
    CHECK(str_is(kind_name(KIND_UINT8), "uint8"));
    CHECK(str_is(kind_name(KIND_UINT16), "uint16"));
    CHECK(str_is(kind_name(KIND_UINT32), "uint32"));
    CHECK(str_is(kind_name(KIND_UINT64), "uint64"));
    CHECK(str_is(kind_name(KIND_UINTPTR), "uintptr"));
    CHECK(str_is(kind_name(KIND_FLOAT32), "float32"));
    CHECK(str_is(kind_name(KIND_FLOAT64), "float64"));
    CHECK(str_is(kind_name(KIND_COMPLEX64), "complex64"));
    CHECK(str_is(kind_name(KIND_COMPLEX128), "complex128"));
    CHECK(str_is(kind_name(KIND_ARRAY), "array"));
    CHECK(str_is(kind_name(KIND_CHAN), "chan"));
    CHECK(str_is(kind_name(KIND_FUNC), "func"));
    CHECK(str_is(kind_name(KIND_INTERFACE), "interface"));
    CHECK(str_is(kind_name(KIND_MAP), "map"));
    /* Go really does print the pointer kind as "ptr" and not as "pointer". */
    CHECK(str_is(kind_name(KIND_POINTER), "ptr"));
    CHECK(str_is(kind_name(KIND_SLICE), "slice"));
    CHECK(str_is(kind_name(KIND_STRING), "string"));
    CHECK(str_is(kind_name(KIND_STRUCT), "struct"));
    CHECK(str_is(kind_name(KIND_UNSAFE_POINTER), "unsafe.Pointer"));
}

TEST(a_kind_off_the_end_does_not_read_off_the_end) {
    CHECK(str_is(kind_name(KIND_MAX), "invalid"));
    CHECK(str_is(kind_name((Kind)9999), "invalid"));
    CHECK(str_is(kind_name((Kind)-1), "invalid"));
}

TEST(the_builtins_have_the_size_c_says_they_have) {
    CHECK_INT_EQ(TYPE_BOOL->size, sizeof(bool));
    CHECK_INT_EQ(TYPE_INT8->size, 1);
    CHECK_INT_EQ(TYPE_INT16->size, 2);
    CHECK_INT_EQ(TYPE_INT32->size, 4);
    CHECK_INT_EQ(TYPE_INT64->size, 8);
    CHECK_INT_EQ(TYPE_UINT8->size, 1);
    CHECK_INT_EQ(TYPE_UINT64->size, 8);
    CHECK_INT_EQ(TYPE_FLOAT32->size, 4);
    CHECK_INT_EQ(TYPE_FLOAT64->size, 8);
    CHECK_INT_EQ(TYPE_COMPLEX64->size, 8);
    CHECK_INT_EQ(TYPE_COMPLEX128->size, 16);
    CHECK_INT_EQ(TYPE_STRING->size, sizeof(Str));
    CHECK_INT_EQ(TYPE_UNSAFE_POINTER->size, sizeof(void *));

    /* Go's int follows the pointer width and so does ours, which is the whole
     * reason Int is a typedef rather than int64_t everywhere. */
    CHECK_INT_EQ(TYPE_INT->size, sizeof(void *));
    CHECK_INT_EQ(TYPE_UINT->size, sizeof(void *));
    CHECK_INT_EQ(TYPE_UINTPTR->size, sizeof(void *));
}

TEST(the_builtins_name_themselves_the_way_go_does) {
    CHECK(str_is(type_name(TYPE_INT), "int"));
    CHECK(str_is(type_name(TYPE_STRING), "string"));
    CHECK(str_is(type_name(TYPE_UNSAFE_POINTER), "unsafe.Pointer"));
    CHECK(str_is(type_name(NULL), "invalid"));

    /* A builtin has no package, which is what makes it a builtin. */
    CHECK_INT_EQ(TYPE_INT->pkg_path.len, 0);
}

/* byte and rune are aliases in Go, not distinct types, and reflect reports them
 * as uint8 and int32. Anything else would be a nicer library and a less
 * faithful one. */
TEST(byte_and_rune_are_aliases_and_not_types) {
    CHECK(TYPE_BYTE == TYPE_UINT8);
    CHECK(TYPE_RUNE == TYPE_INT32);
    CHECK(str_is(type_name(TYPE_BYTE), "uint8"));
    CHECK(str_is(type_name(TYPE_RUNE), "int32"));
}

TEST(no_two_builtins_share_a_hash) {
    const Type *all[] = {
        TYPE_BOOL,       TYPE_INT,     TYPE_INT8,
        TYPE_INT16,      TYPE_INT32,   TYPE_INT64,
        TYPE_UINT,       TYPE_UINT8,   TYPE_UINT16,
        TYPE_UINT32,     TYPE_UINT64,  TYPE_UINTPTR,
        TYPE_FLOAT32,    TYPE_FLOAT64, TYPE_COMPLEX64,
        TYPE_COMPLEX128, TYPE_STRING,  TYPE_UNSAFE_POINTER,
    };
    size_t n = sizeof all / sizeof all[0];
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++)
            CHECK(all[i]->hash != all[j]->hash);
    }
}

TEST(the_kind_predicates_group_things_the_way_reflect_does) {
    CHECK(kind_is_signed(KIND_INT));
    CHECK(kind_is_signed(KIND_INT8));
    CHECK(kind_is_signed(KIND_INT64));
    CHECK(!kind_is_signed(KIND_UINT));
    CHECK(!kind_is_signed(KIND_FLOAT64));
    CHECK(!kind_is_signed(KIND_BOOL));

    CHECK(kind_is_unsigned(KIND_UINT));
    CHECK(kind_is_unsigned(KIND_UINT8));
    CHECK(kind_is_unsigned(KIND_UINTPTR));
    CHECK(!kind_is_unsigned(KIND_INT));
    CHECK(!kind_is_unsigned(KIND_FLOAT32));

    CHECK(kind_is_float(KIND_FLOAT32));
    CHECK(kind_is_float(KIND_FLOAT64));
    CHECK(!kind_is_float(KIND_COMPLEX64));
    CHECK(!kind_is_float(KIND_INT));
}

/* A string compares by its bytes and not by its struct, which is the whole
 * reason the ops table exists. Two Str values pointing at different buffers
 * holding the same text are one value in Go, and memcmp on the struct would say
 * they are two. */
TEST(a_string_compares_by_its_bytes_not_by_its_pointer) {
    char a[] = "hello";
    char b[] = "hello";
    /* Two buffers, or the rest of this proves nothing. Spelled with the
     * addresses of the first elements because gcc rejects comparing two arrays
     * directly, on the grounds that people who write it usually meant strcmp. */
    CHECK(&a[0] != &b[0]);

    Str sa = str_from_cstr(a);
    Str sb = str_from_cstr(b);

    CHECK(type_equal(TYPE_STRING, &sa, &sb));
    CHECK(type_hash(TYPE_STRING, &sa, 0) == type_hash(TYPE_STRING, &sb, 0));

    Str sc = BURROW_S("goodbye");
    CHECK(!type_equal(TYPE_STRING, &sa, &sc));
}

TEST(a_plain_type_compares_by_its_bytes) {
    int64_t x = 42, y = 42, z = 43;
    CHECK(type_equal(TYPE_INT64, &x, &y));
    CHECK(!type_equal(TYPE_INT64, &x, &z));
    CHECK(type_hash(TYPE_INT64, &x, 0) == type_hash(TYPE_INT64, &y, 0));
}

TEST(the_seed_changes_the_hash) {
    Str s = BURROW_S("burrow");
    CHECK(type_hash(TYPE_STRING, &s, 1) != type_hash(TYPE_STRING, &s, 2));

    /* An empty string still hashes to something, and to the same something. */
    Str e = BURROW_STR_EMPTY;
    CHECK(type_hash(TYPE_STRING, &e, 7) == type_hash(TYPE_STRING, &e, 7));
}

TEST(copy_and_zero_go_through_the_type) {
    int32_t src = 0x11223344;
    int32_t dst = 0;
    type_copy(TYPE_INT32, &dst, &src);
    CHECK_INT_EQ(dst, 0x11223344);

    type_zero(TYPE_INT32, &dst);
    CHECK_INT_EQ(dst, 0);

    /* A Str zeroes to the empty string, which is Go's zero value for a string
     * and the reason nothing in burrow checks for NULL before reading a len. */
    Str s = BURROW_S("not empty");
    type_zero(TYPE_STRING, &s);
    CHECK(str_is_empty(s));
}

/* Comparability is a compile time question in Go and a runtime one here,
 * because a map with a slice key has to fail somewhere and this is where it
 * finds out. */
TEST(comparability_matches_the_language) {
    CHECK(type_is_comparable(TYPE_INT));
    CHECK(type_is_comparable(TYPE_STRING));
    CHECK(type_is_comparable(TYPE_FLOAT64));
    CHECK(type_is_comparable(TYPE_UNSAFE_POINTER));
    CHECK(!type_is_comparable(NULL));

    static const Type slice_of_int = {
        {NULL, 0}, {NULL, 0}, KIND_SLICE, 24,   8, 0,   0,
        NULL,      NULL,      NULL,       NULL, 0, 100, NULL,
    };
    CHECK(!type_is_comparable(&slice_of_int));

    /* An array is comparable exactly when its element is, and that recurses. */
    static const Type array_of_int = {
        {NULL, 0}, {NULL, 0}, KIND_ARRAY, 80,   8,  0,   0,
        NULL,      NULL,      NULL,       NULL, 10, 101, NULL,
    };
    static const Type array_of_slice = {
        {NULL, 0}, {NULL, 0}, KIND_ARRAY,    240,  8,  0,   0,
        NULL,      NULL,      &slice_of_int, NULL, 10, 101, NULL,
    };
    CHECK(!type_is_comparable(&array_of_slice));
    /* elem is NULL on array_of_int, which is a malformed descriptor, and the
     * answer there is no rather than a crash. */
    CHECK(!type_is_comparable(&array_of_int));
}

/* --------------------------------------------------- a hand built struct */

typedef struct Point {
    Int x;
    Int y;
    Str label;
} Point;

static const Field point_fields[] = {
    {{(const Byte *)"X", 1}, {(const Byte *)"json:\"x\"", 8}, NULL, 0, true, false},
    {{(const Byte *)"Y", 1}, {(const Byte *)"json:\"y\"", 8}, NULL, 0, true, false},
    {{(const Byte *)"label", 5}, {NULL, 0}, NULL, 0, false, false},
};

/* Sorted by name, because type_method_by_name is a binary search and Go sorts
 * a type's methods too. */
static void thunk_noop(void *recv, void **args, void **rets) {
    (void)recv;
    (void)args;
    (void)rets;
}

static const Method point_methods[] = {
    {{(const Byte *)"Add", 3}, NULL, thunk_noop},
    {{(const Byte *)"Scale", 5}, NULL, thunk_noop},
    {{(const Byte *)"String", 6}, NULL, thunk_noop},
};

static const Type point_type = {
    {(const Byte *)"Point", 5},
    {(const Byte *)"image", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(Point),
    (uint16_t)_Alignof(Point),
    3,
    3,
    point_fields,
    point_methods,
    NULL,
    NULL,
    0,
    200,
    NULL,
};

TEST(fields_are_found_by_name) {
    const Field *f = type_field_by_name(&point_type, BURROW_S("Y"));
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(str_is(f->name, "Y"));
        CHECK(f->exported);
    }

    const Field *unexported = type_field_by_name(&point_type, BURROW_S("label"));
    CHECK(unexported != NULL);
    if (unexported != NULL)
        CHECK(!unexported->exported);

    CHECK(type_field_by_name(&point_type, BURROW_S("Z")) == NULL);
    /* Case matters, the way it does in Go. */
    CHECK(type_field_by_name(&point_type, BURROW_S("y")) == NULL);
    CHECK(type_field_by_name(NULL, BURROW_S("Y")) == NULL);
}

TEST(methods_are_found_by_name) {
    /* Every one of them, because a binary search that only works for the middle
     * element is a binary search that passes a one case test. */
    const Method *first = type_method_by_name(&point_type, BURROW_S("Add"));
    const Method *middle = type_method_by_name(&point_type, BURROW_S("Scale"));
    const Method *last = type_method_by_name(&point_type, BURROW_S("String"));

    CHECK(first != NULL && str_is(first->name, "Add"));
    CHECK(middle != NULL && str_is(middle->name, "Scale"));
    CHECK(last != NULL && str_is(last->name, "String"));

    /* Before the first, after the last, and in between two of them. */
    CHECK(type_method_by_name(&point_type, BURROW_S("AAA")) == NULL);
    CHECK(type_method_by_name(&point_type, BURROW_S("Zzz")) == NULL);
    CHECK(type_method_by_name(&point_type, BURROW_S("Mid")) == NULL);

    /* A prefix of a real name is not a real name. */
    CHECK(type_method_by_name(&point_type, BURROW_S("Str")) == NULL);
    CHECK(type_method_by_name(&point_type, BURROW_S("Strings")) == NULL);

    CHECK(type_method_by_name(NULL, BURROW_S("Add")) == NULL);
    CHECK(type_method_by_name(TYPE_INT, BURROW_S("Add")) == NULL);
}

TEST(a_struct_names_itself_and_its_package) {
    CHECK(str_is(type_name(&point_type), "Point"));
    CHECK(str_is(point_type.pkg_path, "image"));
    CHECK_INT_EQ(point_type.kind, KIND_STRUCT);
    CHECK_INT_EQ(point_type.nfield, 3);
    CHECK_INT_EQ(point_type.nmethod, 3);
}

/* A struct is comparable when every field is, and the fields here have NULL
 * types, which is a malformed descriptor and has to answer no. */
TEST(a_struct_with_broken_fields_is_not_comparable) {
    CHECK(!type_is_comparable(&point_type));
}

int main(void) {
    RUN(every_kind_has_gos_name);
    RUN(a_kind_off_the_end_does_not_read_off_the_end);
    RUN(the_builtins_have_the_size_c_says_they_have);
    RUN(the_builtins_name_themselves_the_way_go_does);
    RUN(byte_and_rune_are_aliases_and_not_types);
    RUN(no_two_builtins_share_a_hash);
    RUN(the_kind_predicates_group_things_the_way_reflect_does);
    RUN(a_string_compares_by_its_bytes_not_by_its_pointer);
    RUN(a_plain_type_compares_by_its_bytes);
    RUN(the_seed_changes_the_hash);
    RUN(copy_and_zero_go_through_the_type);
    RUN(comparability_matches_the_language);
    RUN(fields_are_found_by_name);
    RUN(methods_are_found_by_name);
    RUN(a_struct_names_itself_and_its_package);
    RUN(a_struct_with_broken_fields_is_not_comparable);
    return harness_report("type");
}
