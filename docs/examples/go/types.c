#include "burrow/burrow.h"

static void model(Alloc *a) {
    // doc: model
    // type Celsius float64, with a method String() string.
    TypesPackage *pkg =
        types_new_package(a, BURROW_S("example.com/temp"), BURROW_S("temp"));
    TypesTypeName *name =
        types_new_type_name(a, TOKEN_NO_POS, pkg, BURROW_S("Celsius"), NULL);
    TypesNamed *celsius = types_new_named(a, name, types_type(types_typ[TYPES_FLOAT64]),
                                          slice_nil(TYPE_UNSAFE_POINTER));
    TypesVar *recv =
        types_new_param(a, TOKEN_NO_POS, pkg, BURROW_S("c"), types_type(celsius));
    // The tuple keeps the slice, so it goes in the arena too.
    TypesVar **res = BURROW_NEW_N(a, TypesVar *, 1);
    res[0] = types_new_param(a, TOKEN_NO_POS, pkg, BURROW_S(""),
                             types_type(types_typ[TYPES_STRING]));
    TypesTuple *results =
        types_new_tuple(a, slice_from(res, 1, 1, TYPE_UNSAFE_POINTER));
    TypesSignature *sig = types_new_signature(a, recv, NULL, results, false);
    types_named_add_method(
        celsius, types_new_func(a, TOKEN_NO_POS, pkg, BURROW_S("String"), sig));

    // map[string][]temp.Celsius
    TypesType str = types_type(types_typ[TYPES_STRING]);
    TypesType m = types_type(
        types_new_map(a, str, types_type(types_new_slice(a, types_type(celsius)))));

    // The qualifier says how to write a package: in full, or not at all
    // for the one you are in.
    TypesQualifier full = TYPES_NO_QUALIFIER;
    TypesQualifier here = types_relative_to(pkg);
    fmt_println_v(types_type_string(a, m, full));
    fmt_println_v(types_type_string(a, m, here));
    fmt_println_v(
        types_object_string(a, types_object(types_named_method(celsius, 0)), full));
    fmt_println_v(
        types_type_string(a, types_type_underlying(types_type(celsius)), full));
    fmt_println_v("comparable:", types_comparable(types_type(celsius)),
                  types_comparable(m));
    // doc: end
}

static void universe(Alloc *a) {
    // doc: universe
    TypesQualifier full = TYPES_NO_QUALIFIER;
    TypesScope *u = types_universe();
    TypesObject err = types_scope_lookup(u, BURROW_S("error"));
    fmt_println_v(types_object_string(a, err, full));
    fmt_println_v(
        types_type_string(a, types_type_underlying(types_object_type(err)), full));

    // rune is an alias of int32, so the two are identical.
    TypesType rune = types_object_type(types_scope_lookup(u, BURROW_S("rune")));
    fmt_println_v("rune == int32:",
                  types_identical(rune, types_type(types_typ[TYPES_INT32])));

    // The type an untyped constant gets when nothing else says.
    TypesType untyped = types_type(types_typ[TYPES_UNTYPED_FLOAT]);
    fmt_println_v(types_type_string(a, untyped, full), "->",
                  types_type_string(a, types_default(untyped), full));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    model(a);
    universe(a);
    arena_free(&ar);
    return 0;
}

/* Output:
map[string][]example.com/temp.Celsius
map[string][]Celsius
func (example.com/temp.Celsius).String() string
float64
comparable: true false
type error interface{Error() string}
interface{Error() string}
rune == int32: true
untyped float -> float64
*/
