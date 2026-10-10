#include "burrow/burrow.h"

static void package(Alloc *a) {
    // doc: package
    Str src = BURROW_S("// Package greet says hello.\n"
                       "package greet\n"
                       "\n"
                       "import \"fmt\"\n"
                       "\n"
                       "// A Greeter greets people by name.\n"
                       "type Greeter struct{ Name string }\n"
                       "\n"
                       "// New returns a Greeter for name.\n"
                       "func New(name string) *Greeter { return &Greeter{name} }\n"
                       "\n"
                       "// Greet prints a greeting.\n"
                       "func (g *Greeter) Greet() { fmt.Println(\"Hello,\", g.Name) }\n"
                       "\n"
                       "func helper() {}\n");
    Str test = BURROW_S("package greet_test\n"
                        "\n"
                        "import \"example.com/greet\"\n"
                        "\n"
                        "func ExampleGreeter_Greet() {\n"
                        "\tgreet.New(\"world\").Greet()\n"
                        "\t// Output: Hello, world\n"
                        "}\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *files[2] = {NULL, NULL};
    files[0] =
        parser_parse_file(a, fset, BURROW_S("greet.go"), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS, &err);
    if (BURROW_OK(err))
        files[1] = parser_parse_file(a, fset, BURROW_S("greet_test.go"),
                                     BURROW_ANY(TYPE_STRING, &test),
                                     PARSER_PARSE_COMMENTS, &err);
    if (BURROW_FAILED(err))
        return;
    Slice list = slice_from(files, 2, 2, TYPE_AST_FILE_PTR);
    DocPackage *p =
        doc_new_from_files(a, fset, list, BURROW_S("example.com/greet"), 0, &err);
    if (BURROW_FAILED(err))
        return;
    fmt_printf_v("package %s: %s", p->name, p->doc);
    for (Int i = 0; i < p->types.len; i++) {
        DocType *t = BURROW_AT(DocType *, p->types, i);
        fmt_printf_v("type %s: %s", t->name, t->doc);
        for (Int j = 0; j < t->funcs.len; j++) {
            DocFunc *f = BURROW_AT(DocFunc *, t->funcs, j);
            fmt_printf_v("  func %s: %s", f->name, f->doc);
        }
        for (Int j = 0; j < t->methods.len; j++) {
            DocFunc *m = BURROW_AT(DocFunc *, t->methods, j);
            fmt_printf_v("  method (%s) %s: %s", m->recv, m->name, m->doc);
            for (Int k = 0; k < m->examples.len; k++) {
                DocExample *e = BURROW_AT(DocExample *, m->examples, k);
                fmt_printf_v("    example, output %q\n", e->output);
            }
        }
    }
    // doc: end
}

static void synopsis(Alloc *a) {
    // doc: synopsis
    Str text = BURROW_S("Package sort provides primitives for sorting slices and\n"
                        "user-defined collections. It is fast.\n");
    fmt_printf_v("%q\n", doc_synopsis(a, text));
    fmt_printf_v("%q\n", doc_synopsis(a, BURROW_S("Copyright 2009 The Go Authors.")));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    package(arena_allocator(&ar));
    synopsis(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
package greet: Package greet says hello.
type Greeter: A Greeter greets people by name.
  func New: New returns a Greeter for name.
  method (*Greeter) Greet: Greet prints a greeting.
    example, output "Hello, world\n"
"Package sort provides primitives for sorting slices and user-defined collections."
""
*/
