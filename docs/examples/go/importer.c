#include "burrow/burrow.h"

/* Makes a GOPATH in a new directory with one package in it, example/shapes,
 * and returns the directory. */
static Str write_gopath(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("gopath-*"), &err);
    if (BURROW_FAILED(err))
        return BURROW_STR_EMPTY;
    Str pkg = filepath_join_v(a, 4, dir, BURROW_S("src"), BURROW_S("example"),
                              BURROW_S("shapes"));
    (void)os_mkdir_all(pkg, 0755);
    (void)os_write_file(filepath_join_v(a, 2, pkg, BURROW_S("shapes.go")),
                        BURROW_B("// Package shapes has points.\n"
                                 "package shapes\n"
                                 "\n"
                                 "type Point struct{ X, Y float64 }\n"
                                 "\n"
                                 "func (p Point) Add(q Point) Point {\n"
                                 "\treturn Point{p.X + q.X, p.Y + q.Y}\n"
                                 "}\n"
                                 "\n"
                                 "const Origin = \"(0, 0)\"\n"),
                        0644);
    return dir;
}

static void check(Alloc *a, Str gopath) {
    // doc: source
    // Find packages in GOPATH, the way go/build did before modules.
    (void)os_setenv(BURROW_S("GOPATH"), gopath);
    (void)os_setenv(BURROW_S("GO111MODULE"), BURROW_S("off"));

    Str src = BURROW_S("package main\n"
                       "\n"
                       "import \"example/shapes\"\n"
                       "\n"
                       "var p = shapes.Point{1, 2}.Add(shapes.Point{3, 4})\n"
                       "var n = len(shapes.Origin)\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_S("main.go"),
                                   BURROW_ANY(TYPE_STRING, &src), 0, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(error_text(err));
        return;
    }
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);

    // The source importer type checks example/shapes from its source.
    TypesConfig conf = {0};
    conf.importer =
        importer_for_compiler(a, fset, BURROW_S("source"), (ImporterLookup){0});
    TypesPackage *pkg =
        types_config_check(&conf, a, BURROW_S("main"), fset, files, NULL, &err);
    if (BURROW_FAILED(err)) {
        fmt_println_v(error_text(err));
        return;
    }

    Slice imports = types_package_imports(pkg);
    for (Int i = 0; i < imports.len; i++) {
        TypesPackage *imp = BURROW_AT(TypesPackage *, imports, i);
        fmt_println_v("imports", types_package_path(imp));
    }
    TypesScope *scope = types_package_scope(pkg);
    const char *names[] = {"p", "n"};
    for (int i = 0; i < 2; i++) {
        TypesObject obj = types_scope_lookup(scope, str_from_cstr(names[i]));
        fmt_println_v(types_object_string(a, obj, types_relative_to(pkg)));
    }
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str gopath = write_gopath(a);
    if (gopath.len > 0) {
        check(a, gopath);
        (void)os_remove_all(gopath);
    }
    arena_free(&ar);
    return 0;
}

/* Output:
imports example/shapes
var p example/shapes.Point
var n int
*/
