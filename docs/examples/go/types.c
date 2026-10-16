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

static uint64_t hash_of(MaphashHasher hr, MaphashSeed seed, TypesType t) {
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, seed);
    maphash_hasher_hash(hr, &h, &t);
    return maphash_hash_sum64(&h);
}

static TypesType tagged(Alloc *a, Str tag) {
    TypesVar *f = types_new_field(a, TOKEN_NO_POS, NULL, BURROW_S("F"),
                                  types_type(types_typ[TYPES_INT]), false);
    Slice fields = slice_make(a, TYPE_UNSAFE_POINTER, 1, 1);
    *(TypesVar **)slice_at(fields, 0) = f;
    Slice tags = slice_make(a, TYPE_STRING, 1, 1);
    *(Str *)slice_at(tags, 0) = tag;
    return types_type(types_new_struct(a, fields, tags));
}

static void hashing(Alloc *a) {
    // doc: hashing
    MaphashSeed seed = maphash_make_seed();
    MaphashHasher hr = types_hasher_as_hasher((TypesHasher){0});
    MaphashHasher it = types_hasher_ignore_tags_as_hasher((TypesHasherIgnoreTags){0});
    TypesQualifier full = TYPES_NO_QUALIFIER;

    // The same type, made twice.
    TypesType str = types_type(types_typ[TYPES_STRING]);
    TypesType x =
        types_type(types_new_map(a, str, types_type(types_new_slice(a, str))));
    TypesType y =
        types_type(types_new_map(a, str, types_type(types_new_slice(a, str))));
    bool same = x == y;
    bool same_hash = hash_of(hr, seed, x) == hash_of(hr, seed, y);
    bool equal = maphash_hasher_equal(hr, &x, &y);
    fmt_println_v(types_type_string(a, x, full), "same pointer:", same);
    fmt_println_v("same hash:", same_hash, "equal:", equal);

    // Two structs that differ only in a tag.
    TypesType s1 = tagged(a, BURROW_S("json:\"f\""));
    TypesType s2 = tagged(a, BURROW_S("json:\"g\""));
    fmt_println_v(types_type_string(a, s1, full));
    fmt_println_v(types_type_string(a, s2, full));
    bool tags_equal = maphash_hasher_equal(hr, &s1, &s2);
    bool it_equal = maphash_hasher_equal(it, &s1, &s2);
    bool it_hash = hash_of(it, seed, s1) == hash_of(it, seed, s2);
    fmt_println_v("Hasher equal:", tags_equal);
    fmt_println_v("HasherIgnoreTags equal:", it_equal, "same hash:", it_hash);
    // doc: end
}

static void instantiate(Alloc *a) {
    // doc: instantiate
    // type Box[T comparable] struct{ v T }
    TypesPackage *pkg =
        types_new_package(a, BURROW_S("example.com/box"), BURROW_S("box"));
    TypesType comparable =
        types_object_type(types_scope_lookup(types_universe(), BURROW_S("comparable")));
    TypesTypeParam **tps = BURROW_NEW_N(a, TypesTypeParam *, 1);
    tps[0] = types_new_type_param(
        a, types_new_type_name(a, TOKEN_NO_POS, pkg, BURROW_S("T"), NULL), comparable);
    TypesNamed *box = types_new_named(
        a, types_new_type_name(a, TOKEN_NO_POS, pkg, BURROW_S("Box"), NULL), NULL,
        slice_nil(TYPE_UNSAFE_POINTER));
    types_named_set_type_params(box, slice_from(tps, 1, 1, TYPE_UNSAFE_POINTER));
    TypesVar **fields = BURROW_NEW_N(a, TypesVar *, 1);
    fields[0] =
        types_new_field(a, TOKEN_NO_POS, pkg, BURROW_S("v"), types_type(tps[0]), false);
    types_named_set_underlying(
        box,
        types_type(types_new_struct(a, slice_from(fields, 1, 1, TYPE_UNSAFE_POINTER),
                                    slice_nil(TYPE_STRING))));
    TypesQualifier full = TYPES_NO_QUALIFIER;

    // Box[string], made twice through the same context, is one type.
    TypesContext *ctxt = types_new_context(a);
    TypesType *args = BURROW_NEW_N(a, TypesType, 1);
    args[0] = types_type(types_typ[TYPES_STRING]);
    Slice targs = slice_from(args, 1, 1, TYPE_UNSAFE_POINTER);
    TypesType b1 = types_instantiate(a, ctxt, types_type(box), targs, true, NULL);
    TypesType b2 = types_instantiate(a, ctxt, types_type(box), targs, true, NULL);
    bool same = b1 == b2;
    fmt_println_v(types_type_string(a, b1, full), "same instance:", same);
    fmt_println_v(types_type_string(a, types_type_underlying(b1), full));

    // []int is not comparable, so Box[[]int] fails on argument 0.
    TypesType *bad = BURROW_NEW_N(a, TypesType, 1);
    bad[0] = types_type(types_new_slice(a, types_type(types_typ[TYPES_INT])));
    Error err = BURROW_NO_ERROR;
    TypesType b3 =
        types_instantiate(a, ctxt, types_type(box),
                          slice_from(bad, 1, 1, TYPE_UNSAFE_POINTER), true, &err);
    const TypesArgumentError *ae = errors_as(err, TYPE_TYPES_ARGUMENT_ERROR);
    if (ae != NULL) {
        bool none = b3 == NULL;
        fmt_println_v("argument", ae->index, error_text(ae->err), "no type:", none);
    }

    // An interface satisfies comparable but does not implement it.
    TypesInterface *cmp = types_as_interface(types_type_underlying(comparable));
    TypesType empty = types_type(types_new_interface_type(
        a, slice_nil(TYPE_UNSAFE_POINTER), slice_nil(TYPE_UNSAFE_POINTER)));
    bool impl = types_implements(empty, cmp);
    bool sat = types_satisfies(empty, cmp);
    fmt_println_v("any implements comparable:", impl, "satisfies it:", sat);
    // doc: end
}

static void assign(Alloc *a) {
    // doc: assign
    // type MyInt int
    TypesPackage *pkg =
        types_new_package(a, BURROW_S("example.com/num"), BURROW_S("num"));
    TypesType int_t = types_type(types_typ[TYPES_INT]);
    TypesTypeName *name =
        types_new_type_name(a, TOKEN_NO_POS, pkg, BURROW_S("MyInt"), NULL);
    TypesType my_int =
        types_type(types_new_named(a, name, int_t, slice_nil(TYPE_UNSAFE_POINTER)));
    bool asg = types_assignable_to(my_int, int_t);
    bool conv = types_convertible_to(my_int, int_t);
    fmt_println_v("MyInt to int: assign", asg, "convert", conv);

    TypesType str = types_type(types_typ[TYPES_STRING]);
    TypesType bytes = types_type(types_new_slice(a, types_type(types_typ[TYPES_BYTE])));
    asg = types_assignable_to(bytes, str);
    conv = types_convertible_to(bytes, str);
    fmt_println_v("[]byte to string: assign", asg, "convert", conv);

    // A channel can lose a direction when assigned, but not gain one.
    TypesType ch = types_type(types_new_chan(a, TYPES_SEND_RECV, int_t));
    TypesType recv = types_type(types_new_chan(a, TYPES_RECV_ONLY, int_t));
    bool lose = types_assignable_to(ch, recv);
    bool gain = types_assignable_to(recv, ch);
    fmt_println_v("chan int to <-chan int:", lose, "and back:", gain);

    // Untyped nil goes to pointers, but not to int.
    TypesType nil_t = types_type(types_typ[TYPES_UNTYPED_NIL]);
    bool to_ptr = types_assignable_to(nil_t, types_type(types_new_pointer(a, int_t)));
    bool to_int = types_assignable_to(nil_t, int_t);
    fmt_println_v("nil to *int:", to_ptr, "to int:", to_int);
    // doc: end
}

static void report(void *env, const TypesError *e) {
    Alloc *a = env;
    fmt_println_v(types_error_error(e, a));
}

static void check(Alloc *a) {
    // doc: check
    Str src = BURROW_S("package fib\n"
                       "\n"
                       "type S string\n"
                       "\n"
                       "var a, b, c = len(b), S(c), \"hello\"\n"
                       "\n"
                       "func fib(x int) int {\n"
                       "\tif x < 2 {\n"
                       "\t\treturn x\n"
                       "\t}\n"
                       "\treturn fib(x-1) - fib(x-2)\n"
                       "}\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_S("fib.go"),
                                   BURROW_ANY(TYPE_STRING, &src), 0, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(error_text(err));
        return;
    }
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);

    // Record everything the checker finds out.
    TypesInfo info = types_info_all(a);
    TypesPackage *pkg =
        types_config_check(NULL, a, BURROW_S("fib"), fset, files, &info, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(error_text(err));
        return;
    }

    // What the package declares, in name order.
    TypesScope *scope = types_package_scope(pkg);
    Slice names = types_scope_names(a, scope);
    for (Int i = 0; i < names.len; i++) {
        TypesObject obj = types_scope_lookup(scope, BURROW_AT(Str, names, i));
        fmt_println_v(types_object_string(a, obj, types_relative_to(pkg)));
    }

    // The order the package variables are set in.
    for (Int i = 0; i < info.init_order.len; i++) {
        TypesInitializer *init = BURROW_AT(TypesInitializer *, info.init_order, i);
        fmt_println_v("init", types_initializer_string(a, init));
    }

    // An expression, checked in the package scope.
    TypesTypeAndValue tv =
        types_eval(a, fset, pkg, TOKEN_NO_POS, BURROW_S("b + \"!\""), &err);
    if (!BURROW_FAILED(err))
        fmt_println_v("b + \"!\" has type",
                      types_type_string(a, tv.type, types_relative_to(pkg)));
    // doc: end
}

static void check_errors(Alloc *a) {
    // doc: errors
    Str src = BURROW_S("package p\n"
                       "\n"
                       "func f() {\n"
                       "\tx := 1\n"
                       "\tvar s string = 2\n"
                       "}\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_S("p.go"),
                                   BURROW_ANY(TYPE_STRING, &src), 0, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(error_text(err));
        return;
    }
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);

    // With an error function the checker goes on after the first error.
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){report, a};
    (void)types_config_check(&conf, a, BURROW_S("p"), fset, files, NULL, &err);
    fmt_println_v("first:", error_text(err));
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
    hashing(a);
    instantiate(a);
    assign(a);
    check(a);
    check_errors(a);
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
map[string][]string same pointer: false
same hash: true equal: true
struct{F int "json:\"f\""}
struct{F int "json:\"g\""}
Hasher equal: false
HasherIgnoreTags equal: true same hash: true
example.com/box.Box[string] same instance: true
struct{v string}
argument 0 []int does not satisfy comparable no type: true
any implements comparable: false satisfies it: true
MyInt to int: assign false convert true
[]byte to string: assign false convert true
chan int to <-chan int: true and back: false
nil to *int: true to int: false
type S string
var a int
var b S
var c string
func fib(x int) int
init c = "hello"
init b = S(c)
init a = len(b)
b + "!" has type S
p.go:5:17: cannot use 2 (untyped int constant) as string value in variable declaration
p.go:4:2: declared and not used: x
p.go:5:6: declared and not used: s
first: p.go:5:17: cannot use 2 (untyped int constant) as string value in variable declaration
*/
