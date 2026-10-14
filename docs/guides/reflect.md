# Reflection

`burrow/reflect.h` is Go's `reflect` package. It looks at values whose types are not known until the program runs: what kind of thing a value is, what fields a struct has and what its tags say, what is in a slice or a map. It is what `fmt`, `encoding/json` and the other encoders in burrow are built on, and it is there for your own code too.

C has no type information at run time, so reflection only sees types that have a descriptor. The builtins have one, and so does every type declared with `burrow/declare.h`. [The design note](../design/07-reflect.md) explains why it works this way and what to do with a struct from a header you cannot change.

It can read values and write them, make new ones, and make the types to hold them. Converting between types and calling functions are still to come.

## Declaring a type

A struct declared with `BURROW_STRUCT` gets a descriptor next to it, with each field's name, type, offset and tag:

<!-- example: ../examples/reflect/reflect.c#declare -->
```c
#define CONFIG_FIELDS(F, T)                                                            \
    F(T, Str, Name, "json:\"name\"")                                                   \
    F(T, Int, Port, "json:\"port\"")                                                   \
    F(T, bool, Debug, "json:\"debug,omitempty\"")                                      \
    F(T, Strings, Tags, "json:\"tags\"")
BURROW_STRUCT(Config, CONFIG_FIELDS);
```

`TYPE_OF(Config)` is that descriptor, and `BURROW_ANY(TYPE_OF(Config), &c)` is a `Config` as an `Any`, which is what `reflect_value_of` takes.

## Kinds

A `ReflectValue` is a descriptor and a pointer to the bytes. It does not copy anything, so the variable has to outlive the value. Code that handles any type usually switches on the kind and asks for what that kind has:

<!-- example: ../examples/reflect/reflect.c#show -->
```c
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
```

Asking a value for something its kind does not have, `reflect_value_int` of a string say, panics with a `ReflectValueError` naming the call, the way Go's `*ValueError` does. That is a bug in the program, not something to check for.

## Fields and tags

`reflect_type_field` gives a struct field's name, type, tag and offset, and `reflect_value_field` gives its value. `reflect_struct_tag_get` reads one key out of a tag with Go's rules:

<!-- example: ../examples/reflect/reflect.c#fields -->
```c
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
```

That prints:

```text
Config has 4 fields
Name (string, json "name") = "api"
Port (int, json "port") = 8080
Debug (bool, json "debug,omitempty") = false
Tags ([]string, json "tags") = ["web" "eu"]
```

A type declared in C has no package, so its name prints on its own, `Config` where Go would say `main.Config`.

Looking a field up by name follows Go's rules for embedded structs, including the one where two fields with the same name at the same depth hide each other. A name that is not there gives a value that is not valid, rather than a panic:

<!-- example: ../examples/reflect/reflect.c#by-name -->
```c
ReflectValue port = reflect_value_field_by_name(v, BURROW_S("Port"));
ReflectValue missing = reflect_value_field_by_name(v, BURROW_S("Host"));
fmt_printf_v("Port %d, Host valid %t\n", reflect_value_int(port),
             reflect_value_is_valid(missing));
```

That prints:

```text
Port 8080, Host valid false
```

## DeepEqual

`reflect_deep_equal` compares two values all the way down: slices by their elements, maps by their entries, pointers by what they point at. It is the same function as Go's, cycles included, so a structure that points back at itself compares without running forever:

<!-- example: ../examples/reflect/reflect.c#deep-equal -->
```c
Str other_tags[] = {BURROW_S("web"), BURROW_S("eu")};
Config d = c;
d.Tags = slice_from(other_tags, 2, 2, TYPE_STRING);
fmt_printf_v("c == d: %t\n", reflect_deep_equal(BURROW_ANY(TYPE_OF(Config), &c),
                                                BURROW_ANY(TYPE_OF(Config), &d)));
other_tags[1] = BURROW_S("us");
fmt_printf_v("after the change: %t\n",
             reflect_deep_equal(BURROW_ANY(TYPE_OF(Config), &c),
                                BURROW_ANY(TYPE_OF(Config), &d)));
```

That prints:

```text
c == d: true
after the change: false
```

Go's odd cases come along. A NaN is not equal to itself, a nil slice is not equal to an empty one, and two functions are equal only when both are nil.

## Maps and iterators

`reflect_value_map_range` gives a `ReflectMapIter` that walks a map in Go's order, which is to say no particular order. `reflect_value_seq` and `reflect_value_seq2` give the `IterSeq` and `IterSeq2` that Go's `Value.Seq` and `Value.Seq2` do, so a program can range over a number, a string, an array, a slice, a map or a channel it only has as a `ReflectValue`. The values they hand to the loop body are good until the body returns.

## Setting values

A value can only be set when reflection reached it through a pointer, the same rule Go has. `reflect_value_of(BURROW_ANY(TYPE_OF(Config), &c))` looks at `c` but cannot change it, while going through a `*Config` and asking for the element gives a value that can be set, and so can the fields under it:

<!-- example: ../examples/reflect/reflect.c#set -->
```c
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
```

That prints:

```text
settable: v false, e true
Port 9090, 3 tags, last "us"
```

Setting a value that cannot be set panics with Go's message, and so does setting a field that is not exported, or setting a value of the wrong type. `reflect_value_can_set` asks first.

## Making values and types

`reflect_new`, `reflect_zero`, `reflect_make_slice`, `reflect_make_map` and `reflect_make_chan` make values of a type known only at run time, and `reflect_pointer_to`, `reflect_slice_of`, `reflect_array_of`, `reflect_map_of` and `reflect_chan_of` make the types. A type made twice is the same descriptor both times, so two made types can be compared with `==`:

<!-- example: ../examples/reflect/reflect.c#make -->
```c
ReflectType mt = reflect_map_of(TYPE_STRING, TYPE_INT);
ReflectValue m = reflect_make_map(a, mt);
Str key = BURROW_S("port");
ReflectValue kv = reflect_value_of(BURROW_ANY(TYPE_STRING, &key));
reflect_value_set_map_index(m, kv, port);
fmt_printf_v("%s with %d entry, port = %d\n", reflect_type_string(mt),
             reflect_value_len(m),
             reflect_value_int(reflect_value_map_index(m, kv)));
```

That prints:

```text
map[string]int with 1 entry, port = 9090
```
