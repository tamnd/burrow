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

/* Avalanche. Flip one bit of a key, and every bit of the hash should change
 * about half the time. This is the property that a hash either has or does not,
 * and it is the one that catches a mixing step that looks fine and is not: the
 * first version of this hash used a single multiply, passed every other test in
 * this file, and had pairs of bits here that never moved together at all.
 *
 * Two hundred and fifty six base values per pair, so a pair that is genuinely
 * fifty fifty lands inside a tenth of a half about always, and the bound is
 * loose enough that this does not turn into a flaky test on some future
 * platform. A broken hash misses it by much more than that. */
TEST(one_flipped_key_bit_moves_half_the_hash) {
    for (int bit = 0; bit < 64; bit++) {
        for (int out = 0; out < 64; out++) {
            int flips = 0;
            for (int i = 0; i < 256; i++) {
                /* Spread the bases out rather than counting up from zero, so
                 * the test is not only about keys with sixty leading zeroes. */
                uint64_t x = (uint64_t)i * (uint64_t)0x9e3779b97f4a7c15ULL;
                uint64_t y = x ^ ((uint64_t)1 << bit);
                uint64_t hx = type_hash(TYPE_UINT64, &x, 12345);
                uint64_t hy = type_hash(TYPE_UINT64, &y, 12345);
                if (((hx ^ hy) >> out) & 1)
                    flips++;
            }
            CHECK(flips > 256 / 4 && flips < 256 * 3 / 4);
        }
    }
}

/* The distribution, measured the way the map uses it. A map takes the group
 * from the bits above the low seven and the control byte from the low seven, so
 * a hash can be excellent overall and still be useless if either of those two
 * slices is lumpy.
 *
 * A thousand and twenty four keys into a hundred and twenty eight buckets
 * averages eight. A perfect hash is not expected, and a random one puts up to
 * about twenty in the fullest bucket often enough that a tighter bound than
 * this would fail on a different seed. What this catches is the real failure,
 * which is a hash that leaves whole buckets empty because some input bits never
 * reach the bucket index. */
static void check_spread(const uint64_t *h, int n) {
    int groups[128] = {0};
    int ctrl[128] = {0};
    int gempty = 0, cempty = 0, gmax = 0, cmax = 0;

    for (int i = 0; i < n; i++) {
        groups[(h[i] >> 7) & 127]++;
        ctrl[h[i] & 127]++;
    }
    for (int i = 0; i < 128; i++) {
        if (groups[i] == 0)
            gempty++;
        if (ctrl[i] == 0)
            cempty++;
        if (groups[i] > gmax)
            gmax = groups[i];
        if (ctrl[i] > cmax)
            cmax = ctrl[i];
    }

    CHECK(gmax <= 24);
    CHECK(cmax <= 24);
    CHECK(gempty <= 8);
    CHECK(cempty <= 8);
}

TEST(the_hash_spreads_the_keys_a_map_actually_gets) {
    enum { N = 1024 };
    static uint64_t h[N];
    char buf[32];

    /* Counting numbers, which is what a map keyed by an index or an id holds
     * and the case a weak hash fails on first. */
    for (int i = 0; i < N; i++) {
        Int k = i;
        h[i] = type_hash(TYPE_INT, &k, 7);
    }
    check_spread(h, N);

    /* Multiples of sixteen, which is what a map keyed by a pointer holds. */
    for (int i = 0; i < N; i++) {
        Int k = (Int)i * 16;
        h[i] = type_hash(TYPE_INT, &k, 7);
    }
    check_spread(h, N);

    /* Keys that differ only in their last few bytes, which is every table
     * keyed by a name with a common prefix. */
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof buf, "some/long/prefix/key%04d", i);
        Str s = str_from_cstr(buf);
        h[i] = type_hash(TYPE_STRING, &s, 7);
    }
    check_spread(h, N);

    /* Short strings, where there is the least input to work with. */
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof buf, "%d", i);
        Str s = str_from_cstr(buf);
        h[i] = type_hash(TYPE_STRING, &s, 7);
    }
    check_spread(h, N);

    /* Floats, which go through their own hash on the way to the same mixer. */
    for (int i = 0; i < N; i++) {
        double d = (double)i;
        h[i] = type_hash(TYPE_FLOAT64, &d, 7);
    }
    check_spread(h, N);
}

/* Every length up to a bit past the sixteen byte boundary, because the short
 * path, the overlapping tail read and the loop all meet there and an off by one
 * in any of them shows up as two different lengths hashing the same or as a
 * read outside the key. The second half is what the sanitiser build is for, and
 * this is what gives it something to look at. */
TEST(every_short_length_hashes_to_its_own_number) {
    unsigned char buf[40];
    uint64_t seen[40];

    for (int i = 0; i < 40; i++)
        buf[i] = (unsigned char)(i + 1);

    for (int n = 0; n < 40; n++) {
        Str s = {buf, n};
        seen[n] = type_hash(TYPE_STRING, &s, 3);
        for (int m = 0; m < n; m++)
            CHECK(seen[m] != seen[n]);
    }

    /* And the same bytes with one changed anywhere still moves the answer, at
     * every length, which is the tail read doing its job. */
    for (int n = 1; n < 40; n++) {
        Str s = {buf, n};
        uint64_t before = type_hash(TYPE_STRING, &s, 3);
        for (int i = 0; i < n; i++) {
            buf[i] = (unsigned char)(buf[i] ^ 0x40);
            CHECK(type_hash(TYPE_STRING, &s, 3) != before);
            buf[i] = (unsigned char)(buf[i] ^ 0x40);
        }
    }
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
    RUN(one_flipped_key_bit_moves_half_the_hash);
    RUN(the_hash_spreads_the_keys_a_map_actually_gets);
    RUN(every_short_length_hashes_to_its_own_number);
    RUN(copy_and_zero_go_through_the_type);
    RUN(comparability_matches_the_language);
    RUN(fields_are_found_by_name);
    RUN(methods_are_found_by_name);
    RUN(a_struct_names_itself_and_its_package);
    RUN(a_struct_with_broken_fields_is_not_comparable);
    return harness_report("type");
}
