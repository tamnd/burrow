/* structs has nothing to test beyond the marker being usable the way Go code
 * uses it, and its descriptor saying what it is.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/structs.h"

#include "burrow/testing.h"

#include "check.h"

#include <stddef.h>

typedef struct Marked {
    STRUCTS_HOST_LAYOUT
    uint16_t family;
    Byte data[14];
} Marked;

typedef struct Plain {
    uint16_t family;
    Byte data[14];
} Plain;

static void TestHostLayout(TestingT *t) {
    /* The field form changes nothing about the layout. */
    CHECK_INT_EQ((Int)sizeof(Marked), (Int)sizeof(Plain));
    CHECK_INT_EQ((Int)offsetof(Marked, data), (Int)offsetof(Plain, data));
    const Type *ty = TYPE_STRUCTS_HOST_LAYOUT;
    CHECK(ty->kind == KIND_STRUCT);
    CHECK(str_eq(ty->name, BURROW_S("HostLayout")));
    CHECK(str_eq(ty->pkg_path, BURROW_S("structs")));
    CHECK(type_is_comparable(ty));
    StructsHostLayout a = {0}, b = {0};
    CHECK(type_equal(ty, &a, &b));
}

#define TESTS(X) X(TestHostLayout)

TESTING_MAIN(TESTS)
