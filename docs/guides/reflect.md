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

## Conversions

`reflect_value_convert` does what a Go conversion `T(x)` does, for a type known only at run time. Numbers convert to each other the way they do in Go, a string goes to and from a `[]byte` or a `[]rune`, a slice goes to an array or a pointer to an array of the same element type, and a value goes to a named type with the same underlying type. `reflect_type_convertible_to` asks whether a conversion is allowed at all, and `reflect_type_assignable_to` whether a value can be stored as it is, without one:

<!-- example: ../examples/reflect/reflect.c#convert -->
```c
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
```

That prints:

```text
float64 9090
[]uint8 of length 4
int to *int: false, []byte to [4]byte: true
```

A conversion that is not allowed panics. So does a slice to an array that is longer than the slice, which `reflect_type_convertible_to` cannot see from the types alone, so `reflect_value_can_convert` checks the value too. Converting to an interface with methods takes the vtable the type recorded for it, which the next sections come to.

## Calls and methods

`reflect_value_call` calls a function value with arguments given as a Slice of `ReflectValue`, and gives back its results the same way. A function type needs a descriptor for that, and `BURROW_FUNC_TYPE_DEFINE` makes one from a signature list spelled the way a method's is in `burrow/declare.h`. The descriptor carries a call thunk, which is what reflect calls through. `reflect_make_func` goes the other way and makes a function of a given type out of a C function that takes and returns Values. Methods declared with `BURROW_STRUCT_DEFINE_METHODS` are found with `reflect_value_method_by_name`, which gives back a method value with the receiver already bound:

<!-- example: ../examples/reflect/reflect.c#func-type -->
```c
/* A function type reflect can call, and a struct with a method. */
BURROW_FUNC(Scale, Int, Int x, Int by);
#define SCALE_SIG(IN, OUT) IN(0, Int) IN(1, Int) OUT(Int)
BURROW_FUNC_TYPE_DEFINE(Scale, SCALE_SIG);

static Int scale(void *env, Int x, Int by) {
    (void)env;
    return x * by;
}

#define TEMP_FIELDS(F, T) F(T, double, C, "")
BURROW_STRUCT_DECL(Temp, TEMP_FIELDS);

static double temp_fahrenheit(Temp *t) {
    return t->C * 9 / 5 + 32;
}

#define TEMP_SIG_Fahrenheit(IN, OUT) OUT(double)
#define TEMP_METHODS(M, T) M(T, Fahrenheit, temp_fahrenheit, TEMP_SIG_Fahrenheit)
BURROW_STRUCT_DEFINE_METHODS(Temp, TEMP_FIELDS, TEMP_METHODS);

/* What MakeFunc wraps: it adds its two arguments. The result has to outlast
 * the call, so it lives in the env. */
typedef struct Sum {
    Int n;
    ReflectValue out;
} Sum;

static Slice add_impl(void *env, Slice args) {
    Sum *s = env;
    const ReflectValue *in = args.p;
    s->n = reflect_value_int(in[0]) + reflect_value_int(in[1]);
    s->out = reflect_value_of(BURROW_ANY(TYPE_INT, &s->n));
    return slice_from(&s->out, 1, 1, TYPE_REFLECT_VALUE);
}
```

Then, in a function with an allocator `a`:

<!-- example: ../examples/reflect/reflect.c#call -->
```c
Scale sc = BURROW_FN(Scale, scale, NULL);
ReflectValue fv = reflect_value_of(BURROW_ANY(TYPE_OF(Scale), &sc));
Int x = 21, by = 2;
ReflectValue args[] = {reflect_value_of(BURROW_ANY(TYPE_INT, &x)),
                       reflect_value_of(BURROW_ANY(TYPE_INT, &by))};
Slice in = slice_from(args, 2, 2, TYPE_REFLECT_VALUE);
Slice out = reflect_value_call(fv, a, in);
fmt_printf_v("%s returned %d\n", reflect_type_string(reflect_value_type(fv)),
             reflect_value_int(((ReflectValue *)out.p)[0]));

Sum sum = {0};
ReflectValue add = reflect_make_func(
    a, TYPE_OF(Scale), BURROW_FN(ReflectMakeFuncImpl, add_impl, &sum));
out = reflect_value_call(add, a, in);
fmt_printf_v("made func returned %d\n",
             reflect_value_int(((ReflectValue *)out.p)[0]));

Temp temp = {100};
ReflectValue tmv = reflect_value_of(BURROW_ANY(TYPE_OF(Temp), &temp));
ReflectValue fm = reflect_value_method_by_name(tmv, a, BURROW_S("Fahrenheit"));
out = reflect_value_call(fm, a, slice_nil(TYPE_REFLECT_VALUE));
fmt_printf_v("%d method, %s = %v\n", reflect_value_num_method(tmv),
             reflect_type_string(reflect_value_type(fm)),
             reflect_value_float(((ReflectValue *)out.p)[0]));
```

That prints:

```text
func(int, int) int returned 42
made func returned 23
1 method, func() float64 = 212
```

`reflect_value_call_slice` passes the last argument of a variadic function as a slice, as `f(xs...)` does in Go, and `reflect_func_of` makes a function type at run time. A function made by `reflect_make_func`, or a method value, has no C function of the right signature behind it, so it can be called through reflect and stored where a function of its type goes, but calling it from C panics. Every receiver is a pointer in burrow, so a struct and a pointer to it have the same methods, in name order.

## Interfaces

An interface with methods is a vtable and a data pointer in burrow, and a vtable is C function pointers written by hand, so reflect cannot make one the way Go does. `BURROW_IMPLEMENTS(T, I, vt)` in `burrow/type.h` records the vtable a type has for an interface, and that record is what Set, Call, Convert and `reflect_type_assert` use to put a value into one. `BURROW_IFACE_TYPE_DEFINE` in `burrow/declare.h` makes an interface's descriptor from a list of its methods, spelled the way a struct's methods are, so that reflect can count them and call them on an interface value:

<!-- example: ../examples/reflect/reflect.c#iface-type -->
```c
/* An interface with Temp's one method, and Temp's vtable for it. */
typedef struct FahrenheiterVT {
    const Type *self_type;
    double (*fahrenheit)(void *self);
} FahrenheiterVT;

typedef struct Fahrenheiter {
    const FahrenheiterVT *vt;
    void *data;
} Fahrenheiter;

#define FAHRENHEITER_METHODS(M, T) M(T, Fahrenheit, fahrenheit, TEMP_SIG_Fahrenheit)
BURROW_IFACE_TYPE_DECL(Fahrenheiter);
BURROW_IFACE_TYPE_DEFINE(Fahrenheiter, FAHRENHEITER_METHODS);

static double temp_fahrenheit_of(void *t) {
    return temp_fahrenheit(t);
}

static const FahrenheiterVT temp_fahrenheiter = {TYPE_OF(Temp), temp_fahrenheit_of};
BURROW_IMPLEMENTS(Temp, Fahrenheiter, temp_fahrenheiter);
```

Then `reflect_type_implements` answers for it, `reflect_type_assert` puts a Temp into one, and so does setting an interface variable through reflect:

<!-- example: ../examples/reflect/reflect.c#iface -->
```c
ReflectType ft = TYPE_OF(Fahrenheiter);
fmt_printf_v("Temp: %t, int: %t\n", reflect_type_implements(TYPE_OF(Temp), ft),
             reflect_type_implements(TYPE_INT, ft));

Fahrenheiter fh;
if (reflect_type_assert(tmv, ft, &fh))
    fmt_printf_v("asserted, %v\n", fh.vt->fahrenheit(fh.data));

Fahrenheiter held = {0};
Fahrenheiter *hp = &held;
ReflectValue hv =
    reflect_value_elem(reflect_value_of(BURROW_ANY(reflect_pointer_to(ft), &hp)));
reflect_value_set(hv, tmv);
ReflectValue hm = reflect_value_method(hv, a, 0);
out = reflect_value_call(hm, a, slice_nil(TYPE_REFLECT_VALUE));
fmt_printf_v("%s holds %s, method gives %v\n",
             reflect_type_string(reflect_value_type(hv)),
             reflect_type_string(reflect_value_type(reflect_value_elem(hv))),
             reflect_value_float(((ReflectValue *)out.p)[0]));
```

That prints:

```text
Temp: true, int: false
asserted, 212
Fahrenheiter holds Temp, method gives 212
```

The first field of the vtable is the descriptor of the type it is for, which is how reflect tells what an interface value holds. A type that has every method an interface's descriptor lists implements it, as in Go, even with no vtable recorded, but putting a value of it into the interface panics and says which record is missing. io.Reader and io.Writer list their methods this way, and bytes.Buffer, bytes.Reader, strings.Reader, strings.Builder, bufio.Reader and bufio.Writer record their vtables for them. Every receiver is a pointer in burrow, so `T` implements whatever `*T` does, and an interface holding a pointer gives the value it points at to `reflect_value_elem`.

## Making struct types

`reflect_struct_of` makes a struct type from a Slice of `ReflectStructField`, reading each field's name, package path, type, tag and `anonymous`. It lays the fields out the way C would, and a struct made twice from the same fields is the same descriptor:

<!-- example: ../examples/reflect/reflect.c#struct-of -->
```c
ReflectStructField pf[] = {
    {.name = BURROW_S("X"), .type = TYPE_INT, .tag = BURROW_S("json:\"x\"")},
    {.name = BURROW_S("Y"), .type = TYPE_INT, .tag = BURROW_S("json:\"y\"")},
};
Slice pfs = slice_from(pf, 2, 2, TYPE_REFLECT_STRUCT_FIELD);
ReflectType st = reflect_struct_of(pfs);
ReflectValue sv = reflect_value_elem(reflect_new(a, st));
reflect_value_set_int(reflect_value_field_by_name(sv, BURROW_S("Y")), 7);
bool same = reflect_struct_of(pfs) == st;
fmt_printf_v("%s\n%v, same type again: %t\n", reflect_type_string(st),
             reflect_value_interface(sv), same);
```

That prints:

```text
struct { X int "json:\"x\""; Y int "json:\"y\"" }
{0 7}, same type again: true
```

It panics where Go's StructOf does, with Go's messages. A struct gets the methods of an embedded first field, and of an embedded interface wherever it is, though calling one of the interface's panics, as in Go. Two rules of burrow's own also apply. A field is embedded when it is named for its type, so an anonymous field with any other name panics. And a field is exported when its name starts with an ASCII upper case letter, so a package path on an exported name panics too. No vtable can be made at run time, so a made struct goes into an interface with methods only through reflect: `reflect_type_implements` answers for it, and `reflect_value_method_by_name` calls its methods.
