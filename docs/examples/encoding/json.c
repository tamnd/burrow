#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/mem/arena.h"

BURROW_MAP_TYPE(Scores, Str, Int);

// doc: types
#define PLAYER_FIELDS(F, T)                                                            \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Level, "json:\"level\"")                                                 \
    F(T, Scores, Scores, "json:\"scores\"")
BURROW_STRUCT(Player, PLAYER_FIELDS);
// doc: end

static void show(const char *what, Slice s) {
    printf("%s: %.*s\n", what, (int)s.len, (const char *)s.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: marshal
    Player p = {BURROW_S("ada <admin>"), 3, map_make(a, TYPE_OF(Str), TYPE_INT, 2)};
    BURROW_MAP_SET(Str, Int, p.Scores, BURROW_S("b"), 20);
    BURROW_MAP_SET(Str, Int, p.Scores, BURROW_S("a"), 10);
    Error err = BURROW_NO_ERROR;
    Slice out = json_marshal(a, BURROW_ANY(TYPE_OF(Player), &p), &err);
    // doc: end
    show("marshal", out);

    // doc: unmarshal
    Player q = {0};
    err = json_unmarshal(a, BURROW_B("{\"name\":\"grace\",\"LEVEL\":7}"),
                         BURROW_ANY(TYPE_OF(Player), &q));
    // doc: end
    printf("unmarshal: " BURROW_STR_FMT " %d, err %d\n", BURROW_STR_ARG(q.Name),
           (int)q.Level, BURROW_FAILED(err));

    // doc: errors
    err = json_unmarshal(a, BURROW_B("{\"level\":\"high\"}"),
                         BURROW_ANY(TYPE_OF(Player), &q));
    const JsonUnmarshalTypeError *te = errors_as(err, TYPE_JSON_UNMARSHAL_TYPE_ERROR);
    // doc: end
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    if (te != NULL)
        printf("type error: value " BURROW_STR_FMT ", offset %d\n",
               BURROW_STR_ARG(te->value), (int)te->offset);

    err = json_unmarshal(a, BURROW_B("{\"level\":"), BURROW_ANY(TYPE_OF(Player), &q));
    printf("syntax: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: indent
    BytesBuffer buf = BYTES_BUFFER(a);
    Slice src = BURROW_B("{\"a\": [1, 2], \"b\": {}}");
    bool ok = json_valid(src);
    err = json_indent(&buf, src, BURROW_S(""), BURROW_S("  "));
    // doc: end
    printf("valid: %d, err %d\n", ok, BURROW_FAILED(err));
    show("indent", bytes_buffer_bytes(&buf));

    arena_free(&ar);
    return 0;
}
