/* Tests for burrow-gen from-go, run against what it wrote for its fixture.
 *
 * Not in Go. tests/fromgo/inventory.go is a Go package with one of most things
 * from-go handles in it, and tests/fromgo/inventory_gen.c and inventory_gen.h
 * are its output, checked in and kept current by tools/check-gen.sh. What is
 * checked here is that the output says what the Go says: the fields in Go's
 * order under Go's names with Go's tags, the kinds Go would report, constants
 * with Go's values, and a named string that compares as a string.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "fromgo/inventory_gen.c"

#include "burrow/core.h"
#include "burrow/mem/arena.h"
#include "burrow/type.h"

#include "check.h"

#include <stddef.h>
#include <string.h>

static bool named(const Type *ty, const char *name) {
    return ty != NULL && str_eq(ty->name, str_from_cstr(name));
}

static void TestAStructIsItsGoFieldsInGoOrder(TestingT *t) {
    static const struct {
        const char *name;
        size_t offset;
    } want[] = {
        {"ID", offsetof(InventoryItem, id)},
        {"Name", offsetof(InventoryItem, name)},
        {"Kind", offsetof(InventoryItem, kind)},
        {"Tags", offsetof(InventoryItem, tags)},
        {"Counts", offsetof(InventoryItem, counts)},
        {"Code", offsetof(InventoryItem, code)},
        {"Next", offsetof(InventoryItem, next)},
        {"Added", offsetof(InventoryItem, added)},
        {"Life", offsetof(InventoryItem, life)},
        {"Raw", offsetof(InventoryItem, raw)},
        {"Extra", offsetof(InventoryItem, extra)},
        {"weight", offsetof(InventoryItem, weight)},
        {"_1", offsetof(InventoryItem, _1)},
        {"Default", offsetof(InventoryItem, default_)},
    };
    const Type *ty = TYPE_OF(InventoryItem);

    CHECK(named(ty, "InventoryItem"));
    CHECK(ty->kind == KIND_STRUCT);
    CHECK_INT_EQ(ty->size, sizeof(InventoryItem));
    CHECK_INT_EQ(ty->nfield, sizeof want / sizeof want[0]);
    for (size_t i = 0; i < ty->nfield && i < sizeof want / sizeof want[0]; i++) {
        CHECK(str_eq(ty->fields[i].name, str_from_cstr(want[i].name)));
        CHECK_INT_EQ(ty->fields[i].offset, want[i].offset);
    }
}

static void TestTheTagsAreGosTags(TestingT *t) {
    const Type *ty = TYPE_OF(InventoryItem);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    CHECK(str_eq(ty->fields[0].tag, BURROW_S("json:\"id\"")));
    Str v = tag_get(a, ty->fields[1].tag, BURROW_S("json"));
    CHECK(str_eq(v, BURROW_S("name,omitempty")));
    arena_free(&ar);
    CHECK_INT_EQ(ty->fields[2].tag.len, 0);
}

static void TestExportednessFollowsTheGoNames(TestingT *t) {
    const Type *ty = TYPE_OF(InventoryItem);

    CHECK(field_is_exported(type_field_by_name(ty, BURROW_S("Default"))));
    CHECK(!field_is_exported(type_field_by_name(ty, BURROW_S("weight"))));
    CHECK(field_is_blank(&ty->fields[12]));
}

static void TestAnEmbeddedFieldIsEmbedded(TestingT *t) {
    const Type *ty = TYPE_OF(InventoryShelf);

    CHECK(field_is_embedded(&ty->fields[0]));
    CHECK(ty->fields[0].type == TYPE_OF(InventoryLabel));
    CHECK(!field_is_embedded(&ty->fields[1]));
}

static void TestANamedTypeKeepsItsNameAndItsKind(TestingT *t) {
    CHECK(named(TYPE_OF(InventoryKind), "InventoryKind"));
    CHECK(TYPE_OF(InventoryKind)->kind == KIND_INT);
    CHECK_INT_EQ(TYPE_OF(InventoryKind)->size, sizeof(Int));
    CHECK(TYPE_OF(InventoryKind) != TYPE_OF(Int));
    CHECK(named(TYPE_OF(InventoryLabel), "InventoryLabel"));
    CHECK(TYPE_OF(InventoryLabel)->kind == KIND_STRING);
}

static void TestANamedStringComparesByItsBytes(TestingT *t) {
    char x[] = "shelf", y[] = "shelf";
    InventoryLabel a = {(Byte *)x, 5}, b = {(Byte *)y, 5};
    const Type *ty = TYPE_OF(InventoryLabel);

    CHECK(type_equal(ty, &a, &b));
    CHECK(type_hash(ty, &a, 7) == type_hash(ty, &b, 7));
    y[0] = 'S';
    CHECK(!type_equal(ty, &a, &b));
}

static void TestTheCompositesAreWhatTheGoSays(TestingT *t) {
    const Type *item = TYPE_OF(InventoryItem), *shelf = TYPE_OF(InventoryShelf);

    const Type *next = type_field_by_name(item, BURROW_S("Next"))->type;
    CHECK(next->kind == KIND_POINTER);
    CHECK(next->elem == item);

    const Type *tags = type_field_by_name(item, BURROW_S("Tags"))->type;
    CHECK(tags->kind == KIND_SLICE);
    CHECK(tags->elem == TYPE_OF(Str));

    const Type *code = type_field_by_name(item, BURROW_S("Code"))->type;
    CHECK(code->kind == KIND_ARRAY);
    CHECK_INT_EQ(code->len, 4);
    CHECK(code->elem == TYPE_OF(Byte));

    const Type *grid = type_field_by_name(shelf, BURROW_S("Grid"))->type;
    CHECK(grid->kind == KIND_ARRAY);
    CHECK_INT_EQ(grid->len, 2);
    CHECK_INT_EQ(grid->elem->len, 3);
    CHECK(grid->elem->elem == TYPE_OF(Int));

    const Type *by_kind = type_field_by_name(shelf, BURROW_S("ByKind"))->type;
    CHECK(by_kind->kind == KIND_MAP);
    CHECK(by_kind->key == TYPE_OF(InventoryKind));
    CHECK(by_kind->elem->kind == KIND_SLICE);
    CHECK(by_kind->elem->elem == item);

    CHECK(type_field_by_name(item, BURROW_S("Raw"))->type == TYPE_OF(Bytes));
    CHECK(type_field_by_name(item, BURROW_S("Added"))->type == TYPE_OF(Time));
    CHECK(type_field_by_name(item, BURROW_S("Life"))->type == TYPE_OF(Duration));
    CHECK(type_field_by_name(item, BURROW_S("Extra"))->type == TYPE_OF(Any));
    CHECK(type_field_by_name(shelf, BURROW_S("LastErr"))->type == TYPE_OF(Error));
}

static void TestTheNamedCompositesAreThere(TestingT *t) {
    CHECK(named(TYPE_OF(InventoryShelves), "InventoryShelves"));
    CHECK(TYPE_OF(InventoryShelves)->elem == TYPE_OF(InventoryShelf));

    /* A map, pointer or array descriptor from declare.h has no name, so these
     * have the right shape and not Go's name. */
    CHECK(TYPE_OF(InventoryIndex)->kind == KIND_MAP);
    CHECK(TYPE_OF(InventoryIndex)->key == TYPE_OF(Str));
    CHECK(TYPE_OF(InventoryIndex)->elem->elem == TYPE_OF(InventoryShelf));
    CHECK(TYPE_OF(InventorySlot)->kind == KIND_ARRAY);
    CHECK_INT_EQ(TYPE_OF(InventorySlot)->len, 8);
    CHECK(TYPE_OF(InventorySlot)->elem == TYPE_OF(uint32_t));
    CHECK_INT_EQ(sizeof(InventorySlot), 8 * sizeof(uint32_t));
}

static void TestAnAliasIsTheSameType(TestingT *t) {
    InventoryItemAlias it = {.id = 7};
    InventoryItem *p = &it;

    CHECK(TYPE_OF(InventoryItemAlias) == TYPE_OF(InventoryItem));
    CHECK_INT_EQ(p->id, 7);
}

static void TestTheConstantsHaveGosValues(TestingT *t) {
    InventoryKind k = INVENTORY_SPARE;

    CHECK_INT_EQ(INVENTORY_TOOL, 0);
    CHECK_INT_EQ(INVENTORY_PART, 1);
    CHECK_INT_EQ(k, 2);
    CHECK_INT_EQ(INVENTORY_MAX_ITEMS, 64);
    CHECK(str_eq(INVENTORY_NAME, BURROW_S("inv\t\"store\"?\?/")));
    CHECK_INT_EQ(INVENTORY_NAME.len, 14);
    CHECK(INVENTORY_RATIO == 0.75);
    CHECK(INVENTORY_BIG == (uint64_t)1 << 63);
    CHECK(!INVENTORY_DEBUG);
}

#define TESTS(X)                                                                       \
    X(TestAStructIsItsGoFieldsInGoOrder)                                               \
    X(TestTheTagsAreGosTags)                                                           \
    X(TestExportednessFollowsTheGoNames)                                               \
    X(TestAnEmbeddedFieldIsEmbedded)                                                   \
    X(TestANamedTypeKeepsItsNameAndItsKind)                                            \
    X(TestANamedStringComparesByItsBytes)                                              \
    X(TestTheCompositesAreWhatTheGoSays)                                               \
    X(TestTheNamedCompositesAreThere)                                                  \
    X(TestAnAliasIsTheSameType)                                                        \
    X(TestTheConstantsHaveGosValues)

TESTING_MAIN(TESTS)
