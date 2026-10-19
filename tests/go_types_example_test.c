/* Derived from Go's src/go/types/example_test.go. Go source: go1.27.1.
 *
 * This file shows examples of basic usage of the go/types API.
 *
 * Not in Go: ExampleScope and ExampleMethodSet import fmt, which burrow
 * imports from source, and an example has no way to skip when Go's sources
 * are not there. So they are tests here, TestExampleScope and
 * TestExampleMethodSet, which write to a buffer, compare it with the example's
 * output and skip without the sources. ExampleInfo imports nothing and is an
 * example.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/format.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/regexp.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* Not in Go: the examples that are tests compare what they wrote with this,
 * after trimming white space from both ends, as go test does. */
static void check_output(TestingT *t, StringsBuilder *b, Str want) {
    Str got = strings_trim_space(strings_builder_string(b));
    want = strings_trim_space(want);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got:\n%s\nwant:\n%s", got, want);
}

/* ExampleScope prints the tree of Scopes of a package created from a set of
 * parsed files. */
static void example_scope(IoWriter w) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Parse the source files for a package. */
    TokenFileSet *fset = token_new_file_set(a);
    Slice files = slice_nil(TYPE_AST_FILE_PTR);
    Str srcs[] = {
        S("package main\n"
          "import \"fmt\"\n"
          "func main() {\n"
          "\tfreezing := FToC(-18)\n"
          "\tfmt.Println(freezing, Boiling) }\n"),
        S("package main\n"
          "import \"fmt\"\n"
          "type Celsius float64\n"
          "func (c Celsius) String() string { return fmt.Sprintf(\"%g\xc2\xb0"
          "C\", c) }\n"
          "func FToC(f float64) Celsius { return Celsius(f - 32 / 9 * 5) }\n"
          "const Boiling Celsius = 100\n"
          "func Unused() { {}; {{ var x int; _ = x }} } // make sure empty block "
          "scopes get printed\n"),
    };
    for (Int i = 0; i < NELEM(srcs); i++) {
        AstFile *f = must_parse(a, fset, srcs[i]);
        files = slice_append(a, files, &f, 1);
    }

    /* Type-check a package consisting of these files. Type information for
     * the imported "fmt" package comes from $GOROOT/pkg/$GOOS_$GOOARCH/fmt.a. */
    TypesConfig conf = {.importer = default_importer(fset)};
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg =
        types_config_check(&conf, a, S("temperature"), fset, files, NULL, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));

    /* Print the tree of scopes. For determinism, we redact addresses. */
    StringsBuilder buf = STRINGS_BUILDER(a);
    types_scope_write_to(types_package_scope(pkg), strings_builder_as_io_writer(&buf),
                         0, true);
    Regexp *rx = regexp_must_compile(a, S(" 0x[a-fA-F\\d]*"));
    fmt_fprintln_v(
        w, regexp_replace_all_string(rx, a, strings_builder_string(&buf), S("")));
    regexp_free(rx);
    arena_free(&ar);
}

static void TestExampleScope(TestingT *t) {
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import fmt from");
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    example_scope(strings_builder_as_io_writer(&b));
    check_output(t, &b,
                 S("package \"temperature\" scope {\n"
                   ".  const temperature.Boiling temperature.Celsius\n"
                   ".  type temperature.Celsius float64\n"
                   ".  func temperature.FToC(f float64) temperature.Celsius\n"
                   ".  func temperature.Unused()\n"
                   ".  func temperature.main()\n"
                   ".  main scope {\n"
                   ".  .  package fmt\n"
                   ".  .  function scope {\n"
                   ".  .  .  var freezing temperature.Celsius\n"
                   ".  .  }\n"
                   ".  }\n"
                   ".  main scope {\n"
                   ".  .  package fmt\n"
                   ".  .  function scope {\n"
                   ".  .  .  var c temperature.Celsius\n"
                   ".  .  }\n"
                   ".  .  function scope {\n"
                   ".  .  .  var f float64\n"
                   ".  .  }\n"
                   ".  .  function scope {\n"
                   ".  .  .  block scope {\n"
                   ".  .  .  }\n"
                   ".  .  .  block scope {\n"
                   ".  .  .  .  block scope {\n"
                   ".  .  .  .  .  var x int\n"
                   ".  .  .  .  }\n"
                   ".  .  .  }\n"
                   ".  .  }\n"
                   ".  }\n"
                   "}\n"));
    arena_free(&ar);
}

/* ExampleMethodSet prints the method sets of various types. */
static void example_method_set(IoWriter w) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Parse a single source file. */
    Str input = S("\n"
                  "package temperature\n"
                  "import \"fmt\"\n"
                  "type Celsius float64\n"
                  "func (c Celsius) String() string  { return fmt.Sprintf(\"%g\xc2\xb0"
                  "C\", c) }\n"
                  "func (c *Celsius) SetF(f float64) { *c = Celsius(f - 32 / 9 * 5) }\n"
                  "\n"
                  "type S struct { I; m int }\n"
                  "type I interface { m() byte }\n");
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, input);

    /* Type-check a package consisting of this file. Type information for the
     * imported packages comes from $GOROOT/pkg/$GOOS_$GOOARCH/fmt.a. */
    TypesConfig conf = {.importer = default_importer(fset)};
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg =
        types_config_check(&conf, a, S("temperature"), fset, files, NULL, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));

    /* Print the method sets of Celsius and *Celsius. */
    TypesType celsius =
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("Celsius")));
    TypesType ts[] = {celsius, types_type(types_new_pointer(a, celsius))};
    for (Int i = 0; i < NELEM(ts); i++) {
        fmt_fprintf_v(w, "Method set of %s:\n",
                      types_type_string(a, ts[i], TYPES_NO_QUALIFIER));
        TypesMethodSet *mset = types_new_method_set(a, ts[i]);
        for (Int j = 0; j < types_method_set_len(mset); j++)
            fmt_fprintln_v(w, types_selection_string(a, types_method_set_at(mset, j),
                                                     TYPES_NO_QUALIFIER));
        fmt_fprintf_v(w, "\n");
    }

    /* Print the method set of S. */
    TypesType styp =
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("S")));
    fmt_fprintf_v(w, "Method set of %s:\n",
                  types_type_string(a, styp, TYPES_NO_QUALIFIER));
    fmt_fprintln_v(w, types_method_set_string(a, types_new_method_set(a, styp)));
    arena_free(&ar);
}

static void TestExampleMethodSet(TestingT *t) {
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import fmt from");
    Arena ar;
    arena_init(&ar, NULL, 0);
    StringsBuilder b = STRINGS_BUILDER(arena_allocator(&ar));
    example_method_set(strings_builder_as_io_writer(&b));
    check_output(t, &b,
                 S("Method set of temperature.Celsius:\n"
                   "method (temperature.Celsius) String() string\n"
                   "\n"
                   "Method set of *temperature.Celsius:\n"
                   "method (*temperature.Celsius) SetF(f float64)\n"
                   "method (*temperature.Celsius) String() string\n"
                   "\n"
                   "Method set of temperature.S:\n"
                   "MethodSet {}\n"));
    arena_free(&ar);
}

static Str mode(TypesTypeAndValue tv) {
    if (types_type_and_value_is_void(tv))
        return S("void");
    if (types_type_and_value_is_type(tv))
        return S("type");
    if (types_type_and_value_is_builtin(tv))
        return S("builtin");
    if (types_type_and_value_is_nil(tv))
        return S("nil");
    if (types_type_and_value_assignable(tv)) {
        if (types_type_and_value_addressable(tv))
            return S("var");
        return S("mapindex");
    }
    if (types_type_and_value_is_value(tv))
        return S("value");
    return S("unknown");
}

static Str expr_string(Alloc *a, TokenFileSet *fset, AstExpr expr) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    (void)format_node(a, strings_builder_as_io_writer(&buf), fset,
                      BURROW_ANY(TYPE_AST_EXPR, &expr));
    return strings_builder_string(&buf);
}

/* ExampleInfo prints various facts recorded by the type checker in a
 * types.Info struct: definitions of and references to each named object, and
 * the type, value, and mode of every expression in the package. */
static void ExampleInfo(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Parse a single source file. */
    Str input = S("\n"
                  "package fib\n"
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
                  "}");
    /* We need a specific fileset in this test below for positions. Cannot use
     * typecheck helper. */
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, input);

    /* Type-check the package. We create an empty map for each kind of input
     * we're interested in, and Check populates them. */
    TypesInfo info = {
        .types = map_make(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE, 0),
        .defs = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0),
        .uses = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0),
    };
    TypesConfig conf = {0};
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg =
        types_config_check(&conf, a, S("fib"), fset, files, &info, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err));

    /* Print package-level variables in initialization order.
     *
     * Not in Go: %v of the slice prints each Initializer with its String
     * method, which is done by hand here. */
    Slice inits = slice_nil(TYPE_STRING);
    for (Int i = 0; i < info.init_order.len; i++) {
        Str s = types_initializer_string(
            a, BURROW_AT(TypesInitializer *, info.init_order, i));
        inits = slice_append(a, inits, &s, 1);
    }
    fmt_printf_v("InitOrder: [%s]\n\n", strings_join(a, inits, S(" ")));

    /* For each named object, print the line and column of its definition and
     * each of its uses. */
    fmt_println_v(S("Defs and Uses of each named object:"));
    Map *uses_by_obj = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    Slice objs = slice_nil(TYPE_UNSAFE_POINTER); /* Not in Go: to range over */
    const void *k;
    void *v;
    for (MapIter it = map_iter(info.uses); map_next(&it, &k, &v);) {
        AstIdent *id = *(AstIdent *const *)k;
        TypesObject obj = *(TypesObject *)v;
        TokenPosition posn = token_file_set_position(fset, ast_ident_pos(id));
        Str line_col = fmt_sprintf_v(a, "%d:%d", posn.line, posn.column);
        Slice **uses = map_get(uses_by_obj, &obj);
        if (uses == NULL) {
            Slice *s = BURROW_NEW(a, Slice);
            *s = slice_nil(TYPE_STRING);
            map_set(uses_by_obj, &obj, &s);
            objs = slice_append(a, objs, &obj, 1);
            uses = map_get(uses_by_obj, &obj);
        }
        **uses = slice_append(a, **uses, &line_col, 1);
    }
    Slice items = slice_nil(TYPE_STRING);
    for (Int i = 0; i < objs.len; i++) {
        TypesObject obj = BURROW_AT(TypesObject, objs, i);
        Slice uses = **(Slice **)map_get(uses_by_obj, &obj);
        slices_sort(uses);
        Str item =
            fmt_sprintf_v(a, "%s:\n  defined at %s\n  used at %s",
                          types_object_string(a, obj, types_relative_to(pkg)),
                          token_position_string(
                              token_file_set_position(fset, types_object_pos(obj)), a),
                          strings_join(a, uses, S(", ")));
        items = slice_append(a, items, &item, 1);
    }
    slices_sort(items); /* sort by line:col, in effect */
    fmt_println_v(strings_join(a, items, S("\n")));
    fmt_printf_v("\n");

    fmt_println_v(S("Types and Values of each expression:"));
    items = slice_nil(TYPE_STRING);
    for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
        AstExpr expr = *(AstExpr const *)k;
        TypesTypeAndValue tv = *(const TypesTypeAndValue *)v;
        TokenPosition posn = token_file_set_position(fset, ast_expr_pos(expr));
        Str tvstr = types_type_string(a, tv.type, TYPES_NO_QUALIFIER);
        if (constant_value_kind(tv.value) != CONSTANT_UNKNOWN)
            tvstr =
                fmt_sprintf_v(a, "%s = %s", tvstr, constant_value_string(tv.value, a));
        /* line:col | expr | mode : type = value */
        Str item =
            fmt_sprintf_v(a, "%2d:%2d | %-19s | %-7s : %s", posn.line, posn.column,
                          expr_string(a, fset, expr), mode(tv), tvstr);
        items = slice_append(a, items, &item, 1);
    }
    slices_sort(items);
    fmt_println_v(strings_join(a, items, S("\n")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestExampleScope)                                                                \
    X(TestExampleMethodSet)                                                            \
    X(ExampleInfo, "InitOrder: [c = \"hello\" b = S(c) a = len(b)]\n"                  \
                   "\n"                                                                \
                   "Defs and Uses of each named object:\n"                             \
                   "builtin len:\n"                                                    \
                   "  defined at -\n"                                                  \
                   "  used at 6:15\n"                                                  \
                   "func fib(x int) int:\n"                                            \
                   "  defined at fib:8:6\n"                                            \
                   "  used at 12:20, 12:9\n"                                           \
                   "type S string:\n"                                                  \
                   "  defined at fib:4:6\n"                                            \
                   "  used at 6:23\n"                                                  \
                   "type int:\n"                                                       \
                   "  defined at -\n"                                                  \
                   "  used at 8:12, 8:17\n"                                            \
                   "type string:\n"                                                    \
                   "  defined at -\n"                                                  \
                   "  used at 4:8\n"                                                   \
                   "var b S:\n"                                                        \
                   "  defined at fib:6:8\n"                                            \
                   "  used at 6:19\n"                                                  \
                   "var c string:\n"                                                   \
                   "  defined at fib:6:11\n"                                           \
                   "  used at 6:25\n"                                                  \
                   "var x int:\n"                                                      \
                   "  defined at fib:8:10\n"                                           \
                   "  used at 10:10, 12:13, 12:24, 9:5\n"                              \
                   "\n"                                                                \
                   "Types and Values of each expression:\n"                            \
                   " 4: 8 | string              | type    : string\n"                  \
                   " 6:15 | len                 | builtin : func(fib.S) int\n"         \
                   " 6:15 | len(b)              | value   : int\n"                     \
                   " 6:19 | b                   | var     : fib.S\n"                   \
                   " 6:23 | S                   | type    : fib.S\n"                   \
                   " 6:23 | S(c)                | value   : fib.S\n"                   \
                   " 6:25 | c                   | var     : string\n"                  \
                   " 6:29 | \"hello\"             | value   : string = \"hello\"\n"    \
                   " 8:12 | int                 | type    : int\n"                     \
                   " 8:17 | int                 | type    : int\n"                     \
                   " 9: 5 | x                   | var     : int\n"                     \
                   " 9: 5 | x < 2               | value   : untyped bool\n"            \
                   " 9: 9 | 2                   | value   : int = 2\n"                 \
                   "10:10 | x                   | var     : int\n"                     \
                   "12: 9 | fib                 | value   : func(x int) int\n"         \
                   "12: 9 | fib(x - 1)          | value   : int\n"                     \
                   "12: 9 | fib(x-1) - fib(x-2) | value   : int\n"                     \
                   "12:13 | x                   | var     : int\n"                     \
                   "12:13 | x - 1               | value   : int\n"                     \
                   "12:15 | 1                   | value   : int = 1\n"                 \
                   "12:20 | fib                 | value   : func(x int) int\n"         \
                   "12:20 | fib(x - 2)          | value   : int\n"                     \
                   "12:24 | x                   | var     : int\n"                     \
                   "12:24 | x - 2               | value   : int\n"                     \
                   "12:26 | 2                   | value   : int = 2\n")

TESTING_MAIN(TESTS)
