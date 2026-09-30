#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/mem/arena.h"

BURROW_SLICE_TYPE(Strs, Str);
BURROW_MAP_TYPE(Extra, Str, Any);

// doc: types
#define ITEM_FIELDS(F, T)                                                              \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, double, Price, "json:\"price,string\"")                                       \
    F(T, Strs, Tags, "json:\"tags,omitempty\"")                                        \
    F(T, bool, Hidden, "json:\"-\"")                                                   \
    F(T, Int, Stock, "json:\"stock,omitzero\"")
BURROW_STRUCT(Item, ITEM_FIELDS);
// doc: end

// doc: fallback
#define LOOSE_FIELDS(F, T)                                                             \
    F(T, Str, ID, "json:\"id\"")                                                       \
    F(T, Extra, Rest, "json:\",embed\"")
BURROW_STRUCT(Loose, LOOSE_FIELDS);
// doc: end

static void show(const char *what, Slice s) {
    printf("%s: %.*s\n", what, (int)s.len, (const char *)s.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: marshal
    Str tags[] = {BURROW_S("tea"), BURROW_S("green")};
    Item it = {BURROW_S("sencha"), 12.5, {tags, 2, 2, TYPE_OF(Str)}, true, 0};
    Error err = BURROW_NO_ERROR;
    Slice out = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Item), &it), &err, 0);
    // doc: end
    show("marshal", out);

    // doc: unmarshal
    Item back = {0};
    err = jsonv2_unmarshal_v(
        a, BURROW_B("{\"name\":\"matcha\",\"price\":\"30\",\"stock\":4}"),
        BURROW_ANY(TYPE_OF(Item), &back), 0);
    // doc: end
    printf("unmarshal: " BURROW_STR_FMT " %g %d, err %d\n", BURROW_STR_ARG(back.Name),
           back.Price, (int)back.Stock, BURROW_FAILED(err));

    // doc: any
    Any v = {NULL, NULL};
    err = jsonv2_unmarshal_v(a, BURROW_B("{\"b\":[1,\"two\",null],\"a\":true}"),
                             BURROW_ANY(TYPE_ANY, &v), 0);
    Map *obj = *(Map **)v.data;
    Str key = BURROW_S("b");
    const Any *b = map_get(obj, &key);
    Slice again = jsonv2_marshal_v(a, v, &err, 1, jsonv2_deterministic(true));
    // doc: end
    printf("any: %d members, b is a slice: %d\n", (int)map_len(obj),
           b->t == TYPE_JSONV2_SLICE_ANY);
    show("again", again);

    // doc: errors
    int8_t small = 0;
    err =
        jsonv2_unmarshal_v(a, BURROW_B("300"), BURROW_ANY(TYPE_OF(int8_t), &small), 0);
    // doc: end
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    const Jsonv2SemanticError *se = errors_as(err, TYPE_JSONV2_SEMANTIC_ERROR);
    printf("semantic: %d, offset %d\n", se != NULL,
           se != NULL ? (int)se->byte_offset : -1);

    err = jsonv2_unmarshal_v(a, BURROW_B("{\"name\":\"x\",\"colour\":\"red\"}"),
                             BURROW_ANY(TYPE_OF(Item), &back), 1,
                             jsonv2_reject_unknown_members(true));
    printf("unknown: " BURROW_STR_FMT ", is: %d\n", BURROW_STR_ARG(error_text(err)),
           errors_is(err, jsonv2_err_unknown_name));

    // doc: loose
    Loose l = {0};
    err = jsonv2_unmarshal_v(a, BURROW_B("{\"id\":\"7\",\"size\":2,\"hot\":true}"),
                             BURROW_ANY(TYPE_OF(Loose), &l), 0);
    Slice loose = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Loose), &l), &err, 1,
                                   jsonv2_deterministic(true));
    // doc: end
    printf("loose: %d extra\n", (int)map_len(l.Rest));
    show("loose", loose);

    arena_free(&ar);
    return 0;
}
