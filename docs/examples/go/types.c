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

static void lookup(Alloc *a) {
    // doc: lookup
    // type Inner struct{ N int }, with func (*Inner) Reset(), and
    // type Outer struct{ Inner }.
    TypesPackage *pkg =
        types_new_package(a, BURROW_S("example.com/box"), BURROW_S("box"));
    TypesVar **inner_fields = BURROW_NEW_N(a, TypesVar *, 1);
    inner_fields[0] = types_new_field(a, TOKEN_NO_POS, pkg, BURROW_S("N"),
                                      types_type(types_typ[TYPES_INT]), false);
    TypesStruct *inner_struct = types_new_struct(
        a, slice_from(inner_fields, 1, 1, TYPE_UNSAFE_POINTER), slice_nil(TYPE_STRING));
    TypesNamed *inner = types_new_named(
        a, types_new_type_name(a, TOKEN_NO_POS, pkg, BURROW_S("Inner"), NULL),
        types_type(inner_struct), slice_nil(TYPE_UNSAFE_POINTER));
    TypesType inner_ptr = types_type(types_new_pointer(a, types_type(inner)));
    TypesVar *recv = types_new_param(a, TOKEN_NO_POS, pkg, BURROW_S("in"), inner_ptr);
    TypesSignature *reset_sig = types_new_signature(a, recv, NULL, NULL, false);
    types_named_add_method(
        inner, types_new_func(a, TOKEN_NO_POS, pkg, BURROW_S("Reset"), reset_sig));

    TypesVar **outer_fields = BURROW_NEW_N(a, TypesVar *, 1);
    outer_fields[0] = types_new_field(a, TOKEN_NO_POS, pkg, BURROW_S("Inner"),
                                      types_type(inner), true);
    TypesStruct *outer_struct = types_new_struct(
        a, slice_from(outer_fields, 1, 1, TYPE_UNSAFE_POINTER), slice_nil(TYPE_STRING));
    TypesNamed *outer = types_new_named(
        a, types_new_type_name(a, TOKEN_NO_POS, pkg, BURROW_S("Outer"), NULL),
        types_type(outer_struct), slice_nil(TYPE_UNSAFE_POINTER));
    TypesType outer_t = types_type(outer);
    TypesType outer_ptr = types_type(types_new_pointer(a, outer_t));
    TypesQualifier here = types_relative_to(pkg);

    // o.N is found through the embedded Inner.
    Str n = BURROW_S("N");
    TypesSelection *sel = types_lookup_selection(a, outer_t, false, pkg, n);
    Str sel_str = types_selection_string(a, sel, here);
    fmt_println_v(sel_str, "depth", types_selection_index(sel).len);

    // Reset has a pointer receiver, so o.Reset() needs o to be addressable.
    Str reset = BURROW_S("Reset");
    bool on_value = types_lookup_selection(a, outer_t, false, pkg, reset) != NULL;
    bool on_var = types_lookup_selection(a, outer_t, true, pkg, reset) != NULL;
    fmt_println_v("Reset on a value:", on_value, "on a variable:", on_var);

    // And so it is in the method set of *Outer and not of Outer.
    fmt_println_v("Outer has", types_method_set_len(types_new_method_set(a, outer_t)),
                  "methods");
    fmt_print_v(types_method_set_string(a, types_new_method_set(a, outer_ptr)));

    // Which of interface{ Reset() } does Outer lack?
    TypesFunc **methods = BURROW_NEW_N(a, TypesFunc *, 1);
    methods[0] = types_new_func(a, TOKEN_NO_POS, pkg, reset,
                                types_new_signature(a, NULL, NULL, NULL, false));
    TypesInterface *resetter =
        types_new_interface_type(a, slice_from(methods, 1, 1, TYPE_UNSAFE_POINTER),
                                 slice_nil(TYPE_UNSAFE_POINTER));
    bool wrong_type;
    TypesFunc *missing = types_missing_method(outer_t, resetter, true, &wrong_type);
    Str missing_name = types_object_name(types_object(missing));
    fmt_println_v("Outer lacks", missing_name, "wrong type:", wrong_type);
    bool ptr_ok = types_missing_method(outer_ptr, resetter, true, NULL) == NULL;
    fmt_println_v("*Outer implements it:", ptr_ok);
    // doc: end
}

static void sizes(Alloc *a) {
    // doc: sizes
    // struct{ a bool; b int64; c bool }, and the same fields with b first.
    TypesPackage *pkg =
        types_new_package(a, BURROW_S("example.com/layout"), BURROW_S("layout"));
    TypesType bool_t = types_type(types_typ[TYPES_BOOL]);
    TypesType i64 = types_type(types_typ[TYPES_INT64]);
    TypesVar **loose = BURROW_NEW_N(a, TypesVar *, 3);
    loose[0] = types_new_field(a, TOKEN_NO_POS, pkg, BURROW_S("a"), bool_t, false);
    loose[1] = types_new_field(a, TOKEN_NO_POS, pkg, BURROW_S("b"), i64, false);
    loose[2] = types_new_field(a, TOKEN_NO_POS, pkg, BURROW_S("c"), bool_t, false);
    TypesVar **tight = BURROW_NEW_N(a, TypesVar *, 3);
    tight[0] = loose[1];
    tight[1] = loose[0];
    tight[2] = loose[2];
    Slice fields[2] = {slice_from(loose, 3, 3, TYPE_UNSAFE_POINTER),
                       slice_from(tight, 3, 3, TYPE_UNSAFE_POINTER)};

    TypesQualifier full = TYPES_NO_QUALIFIER;
    const char *arches[] = {"amd64", "386"};
    for (int i = 0; i < 2; i++) {
        TypesSizes sizes = types_sizes_for(BURROW_S("gc"), str_from_cstr(arches[i]));
        for (int j = 0; j < 2; j++) {
            TypesType st =
                types_type(types_new_struct(a, fields[j], slice_nil(TYPE_STRING)));
            fmt_println_v(arches[i], types_type_string(a, st, full));
            fmt_println_v("  size", types_sizes_sizeof(sizes, st), "align",
                          types_sizes_alignof(sizes, st), "offsets",
                          types_sizes_offsetsof(sizes, a, fields[j]));
        }
    }

    // SizesFor only knows the gc and gccgo compilers.
    TypesSizes none = types_sizes_for(BURROW_S("tinygo"), BURROW_S("amd64"));
    bool known = none.vt != NULL;
    fmt_println_v("tinygo known:", known);
    // doc: end
}

static void exprs(Alloc *a) {
    // doc: exprstring
    const char *srcs[] = {
        "f(a, b...)[i:j]",
        "func(x int) string { return \"\" }",
        "map[string]int{\"a\": 1}",
        "x.(interface{ m() int })",
        "-x + y*<-ch",
    };
    for (int i = 0; i < 5; i++) {
        Error err = BURROW_NO_ERROR;
        AstExpr x = parser_parse_expr(a, str_from_cstr(srcs[i]), &err);
        if (!BURROW_OK(err)) {
            fmt_println_v(error_text(err));
            continue;
        }
        fmt_println_v(types_expr_string(a, x));
    }
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    model(a);
    universe(a);
    lookup(a);
    sizes(a);
    exprs(a);
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
field (Outer) N int depth 2
Reset on a value: false on a variable: true
Outer has 0 methods
MethodSet {
	method (*example.com/box.Outer) Reset()
}
Outer lacks Reset wrong type: true
*Outer implements it: true
amd64 struct{a bool; b int64; c bool}
  size 24 align 8 offsets [0 8 16]
amd64 struct{b int64; a bool; c bool}
  size 16 align 8 offsets [0 8 9]
386 struct{a bool; b int64; c bool}
  size 16 align 4 offsets [0 4 12]
386 struct{b int64; a bool; c bool}
  size 12 align 4 offsets [0 8 9]
tinygo known: false
f(a, b...)[i:j]
(func(x int) string literal)
map[string]int{…}
x.(interface{m() int})
-x + y * <-ch
*/
