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

// doc: func-type
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
// doc: end

// doc: iface-type
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

    // doc: call
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
    // doc: end

    // doc: iface
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
    // doc: end

    // doc: struct-of
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
func(int, int) int returned 42
made func returned 23
1 method, func() float64 = 212
Temp: true, int: false
asserted, 212
Fahrenheiter holds Temp, method gives 212
struct { X int "json:\"x\""; Y int "json:\"y\"" }
{0 7}, same type again: true
*/
