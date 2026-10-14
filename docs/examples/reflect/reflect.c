#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/declare.h"

BURROW_SLICE_TYPE(Strings, Str);

// doc: declare
#define CONFIG_FIELDS(F, T)                                                            \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Int, Port, "json:\"port\"")                                                   \
    F(T, bool, Debug, "json:\"debug,omitempty\"")                                      \
    F(T, Strings, Tags, "json:\"tags\"")
BURROW_STRUCT(Config, CONFIG_FIELDS);
// doc: end

// doc: show
/* Prints v the way it would look in Go source, going into slices and structs
 * and asking each value for its kind on the way. */
static void show(ReflectValue v) {
    switch ((int)reflect_value_kind(v)) {
    case KIND_STRING:
        fmt_printf_v("%q", reflect_value_string(v));
        break;
    case KIND_INT:
        fmt_printf_v("%d", reflect_value_int(v));
        break;
    case KIND_BOOL:
        fmt_printf_v("%t", reflect_value_bool(v));
        break;
    case KIND_SLICE:
        fmt_printf_v("[");
        for (Int i = 0; i < reflect_value_len(v); i++) {
            if (i > 0)
                fmt_printf_v(" ");
            show(reflect_value_index(v, i));
        }
        fmt_printf_v("]");
        break;
    default:
        fmt_printf_v("<%s>", reflect_type_string(reflect_value_type(v)));
    }
}
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str tags[] = {BURROW_S("web"), BURROW_S("eu")};
    Config c = {BURROW_S("api"), 8080, false, slice_from(tags, 2, 2, TYPE_STRING)};

    // doc: fields
    ReflectValue v = reflect_value_of(BURROW_ANY(TYPE_OF(Config), &c));
    ReflectType t = reflect_value_type(v);
    fmt_printf_v("%s has %d fields\n", reflect_type_string(t),
                 reflect_value_num_field(v));
    for (Int i = 0; i < reflect_value_num_field(v); i++) {
        ReflectStructField f = reflect_type_field(t, i);
        Str key = reflect_struct_tag_get(f.tag, a, BURROW_S("json"));
        fmt_printf_v("%s (%s, json %q) = ", f.name, reflect_type_string(f.type), key);
        show(reflect_value_field(v, i));
        fmt_printf_v("\n");
    }
    // doc: end

    // doc: by-name
    ReflectValue port = reflect_value_field_by_name(v, BURROW_S("Port"));
    ReflectValue missing = reflect_value_field_by_name(v, BURROW_S("Host"));
    fmt_printf_v("Port %d, Host valid %t\n", reflect_value_int(port),
                 reflect_value_is_valid(missing));
    // doc: end

    // doc: deep-equal
    Str other_tags[] = {BURROW_S("web"), BURROW_S("eu")};
    Config d = c;
    d.Tags = slice_from(other_tags, 2, 2, TYPE_STRING);
    fmt_printf_v("c == d: %t\n", reflect_deep_equal(BURROW_ANY(TYPE_OF(Config), &c),
                                                    BURROW_ANY(TYPE_OF(Config), &d)));
    other_tags[1] = BURROW_S("us");
    fmt_printf_v("after the change: %t\n",
                 reflect_deep_equal(BURROW_ANY(TYPE_OF(Config), &c),
                                    BURROW_ANY(TYPE_OF(Config), &d)));
    // doc: end

    // doc: set
    Config *p = &c;
    ReflectValue e =
        reflect_value_elem(reflect_value_of(BURROW_ANY(reflect_pointer_to(t), &p)));
    fmt_printf_v("settable: v %t, e %t\n", reflect_value_can_set(v),
                 reflect_value_can_set(e));
    reflect_value_set_int(reflect_value_field_by_name(e, BURROW_S("Port")), 9090);
    ReflectValue tv = reflect_value_field_by_name(e, BURROW_S("Tags"));
    Str more = BURROW_S("us");
    ReflectValue extra[] = {reflect_value_of(BURROW_ANY(TYPE_STRING, &more))};
    reflect_value_set(
        tv, reflect_append(a, tv, slice_from(extra, 1, 1, TYPE_REFLECT_VALUE)));
    fmt_printf_v("Port %d, %d tags, last %q\n", c.Port, c.Tags.len,
                 ((Str *)c.Tags.p)[c.Tags.len - 1]);
    // doc: end

    // doc: make
    ReflectType mt = reflect_map_of(TYPE_STRING, TYPE_INT);
    ReflectValue m = reflect_make_map(a, mt);
    Str key = BURROW_S("port");
    ReflectValue kv = reflect_value_of(BURROW_ANY(TYPE_STRING, &key));
    reflect_value_set_map_index(m, kv, port);
    fmt_printf_v("%s with %d entry, port = %d\n", reflect_type_string(mt),
                 reflect_value_len(m),
                 reflect_value_int(reflect_value_map_index(m, kv)));
    // doc: end

    // doc: convert
    ReflectValue f = reflect_value_convert(port, a, TYPE_FLOAT64);
    fmt_printf_v("%s %v\n", reflect_type_string(reflect_value_type(f)),
                 reflect_value_float(f));
    ReflectValue b = reflect_value_convert(kv, a, TYPE_BYTES);
    fmt_printf_v("%s of length %d\n", reflect_type_string(reflect_value_type(b)),
                 reflect_value_len(b));
    fmt_printf_v(
        "int to *int: %t, []byte to [4]byte: %t\n",
        reflect_type_convertible_to(TYPE_INT, reflect_pointer_to(TYPE_INT)),
        reflect_type_convertible_to(TYPE_BYTES, reflect_array_of(4, TYPE_UINT8)));
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
Config has 4 fields
Name (string, json "name") = "api"
Port (int, json "port") = 8080
Debug (bool, json "debug,omitempty") = false
Tags ([]string, json "tags") = ["web" "eu"]
Port 8080, Host valid false
c == d: true
after the change: false
settable: v false, e true
Port 9090, 3 tags, last "us"
map[string]int with 1 entry, port = 9090
float64 9090
[]uint8 of length 4
int to *int: false, []byte to [4]byte: true
*/
