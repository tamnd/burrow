#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json/jsontext.h"
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

// doc: event
#define EVENT_FIELDS(F, T)                                                             \
    F(T, Str, Kind, "json:\"kind\"")                                                   \
    F(T, JsontextValue, Data, "json:\"data\"")
BURROW_STRUCT(Event, EVENT_FIELDS);
// doc: end

// doc: yesno
static Slice yes_no(void *ctx, Alloc *a, Any v, Error *err) {
    (void)ctx;
    (void)a;
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = *(const bool *)v.data ? BURROW_S("\"yes\"") : BURROW_S("\"no\"");
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

static Error parse_yes_no(void *ctx, Alloc *a, Slice data, Any v) {
    (void)ctx;
    (void)a;
    Str s = str_from_bytes(data.p, data.len);
    if (!str_eq(s, BURROW_S("\"yes\"")) && !str_eq(s, BURROW_S("\"no\"")))
        return errors_new(error_allocator(), BURROW_S("want yes or no"));
    *(bool *)v.data = str_eq(s, BURROW_S("\"yes\""));
    return BURROW_NO_ERROR;
}

BURROW_PTR_TYPE(BoolPtr, bool);
BURROW_SLICE_TYPE(Bools, bool);
// doc: end

// doc: methods
#define VERSION_FIELDS(F, T)                                                           \
    F(T, Int, Major, "")                                                               \
    F(T, Int, Minor, "")
BURROW_STRUCT_DECL(Version, VERSION_FIELDS);

static Slice version_marshal_text(Version *v, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str s = fmt_sprintf_v(a, "v%d.%d", v->Major, v->Minor);
    return slice_append(a, slice_nil(TYPE_BYTE), s.p, s.len);
}

static Error version_unmarshal_text(Version *v, Alloc *a, Slice text) {
    (void)a;
    Str s = strings_trim_prefix(str_from_bytes(text.p, text.len), BURROW_S("v"));
    Str minor;
    bool found;
    Str major = strings_cut(s, BURROW_S("."), &minor, &found);
    Error err = BURROW_NO_ERROR;
    if (found)
        v->Major = strconv_atoi(major, &err);
    if (found && BURROW_OK(err))
        v->Minor = strconv_atoi(minor, &err);
    if (!found || BURROW_FAILED(err))
        return errors_new(error_allocator(), BURROW_S("not a version"));
    return BURROW_NO_ERROR;
}

#define VERSION_METHODS(M, T)                                                          \
    M(T, MarshalText, version_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                 \
    M(T, UnmarshalText, version_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(Version, VERSION_FIELDS, VERSION_METHODS);

#define RELEASE_FIELDS(F, T)                                                           \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Version, Version, "json:\"version\"")
BURROW_STRUCT(Release, RELEASE_FIELDS);
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

    // doc: release
    Release r = {BURROW_S("burrow"), {0, 2}};
    Slice rel = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Release), &r), &err, 0);
    Release r2 = {0};
    err = jsonv2_unmarshal_v(a, rel, BURROW_ANY(TYPE_OF(Release), &r2), 0);
    // doc: end
    show("release", rel);
    printf("version: %d.%d, err %d\n", (int)r2.Version.Major, (int)r2.Version.Minor,
           BURROW_FAILED(err));
    err = jsonv2_unmarshal_v(a, BURROW_B("{\"version\":\"two\"}"),
                             BURROW_ANY(TYPE_OF(Release), &r2), 0);
    printf("bad version: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: funcs
    Jsonv2Marshalers *ms = jsonv2_marshal_func(a, TYPE_BOOL, yes_no, NULL);
    Jsonv2Unmarshalers *us =
        jsonv2_unmarshal_func(a, TYPE_OF(BoolPtr), parse_yes_no, NULL);
    bool flags[] = {true, false};
    Slice fl = {flags, 2, 2, TYPE_BOOL};
    Slice words = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Bools), &fl), &err, 1,
                                   jsonv2_with_marshalers(ms));
    Slice fl2 = slice_nil(TYPE_BOOL);
    err = jsonv2_unmarshal_v(a, BURROW_B("[\"no\",\"maybe\"]"),
                             BURROW_ANY(TYPE_OF(Bools), &fl2), 1,
                             jsonv2_with_unmarshalers(us));
    // doc: end
    show("words", words);
    printf("maybe: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    // doc: raw
    Event ev = {0};
    err = jsonv2_unmarshal_v(
        a, BURROW_B("{\"kind\":\"click\", \"data\": {\"x\": 1, \"y\": 2}}"),
        BURROW_ANY(TYPE_OF(Event), &ev), 0);
    Slice evb = jsonv2_marshal_v(a, BURROW_ANY(TYPE_OF(Event), &ev), &err, 0);
    // doc: end
    show("data", ev.Data);
    show("event", evb);

    arena_free(&ar);
    return 0;
}
