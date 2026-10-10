#include "burrow/burrow.h"

static void fprint(Alloc *a) {
    // doc: fprint
    Str src = BURROW_S("package main\n"
                       "import \"fmt\"\n"
                       "func main() {\n"
                       "fmt.Println( \"hello\" ,1+2 ) // say hello\n"
                       "}\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, BURROW_S("hello.go"), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("parse: %s\n", error_text(err));
        return;
    }
    StringsBuilder b = STRINGS_BUILDER(a);
    err = printer_fprint(a, strings_builder_as_io_writer(&b), fset,
                         BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    if (BURROW_FAILED(err)) {
        fmt_printf_v("print: %s\n", error_text(err));
        return;
    }
    fmt_printf_v("%s", strings_builder_string(&b));
    // doc: end
}

static void config(Alloc *a) {
    // doc: config
    Str src = BURROW_S("package p\n"
                       "type Person struct {\n"
                       "Name string // full name\n"
                       "Age int // in years\n"
                       "}\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, BURROW_STR_EMPTY, BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS, &err);
    if (BURROW_FAILED(err))
        return;
    // Tabs to indent and spaces to align, the way gofmt prints.
    PrinterConfig cfg = {PRINTER_USE_SPACES | PRINTER_TAB_INDENT, 8, 0};
    StringsBuilder b = STRINGS_BUILDER(a);
    err = printer_config_fprint(&cfg, a, strings_builder_as_io_writer(&b), fset,
                                BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    if (BURROW_OK(err))
        fmt_printf_v("%s", strings_builder_string(&b));
    // doc: end
}

static void expr(Alloc *a) {
    // doc: expr
    Error err = BURROW_NO_ERROR;
    AstExpr x = parser_parse_expr(a, BURROW_S("a*(b+c)  ==  f(x,y)[ 0 ]"), &err);
    if (BURROW_FAILED(err))
        return;
    StringsBuilder b = STRINGS_BUILDER(a);
    err = printer_fprint(a, strings_builder_as_io_writer(&b), token_new_file_set(a),
                         BURROW_ANY(TYPE_AST_EXPR, &x));
    if (BURROW_OK(err))
        fmt_printf_v("%s\n", strings_builder_string(&b));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    fprint(arena_allocator(&ar));
    config(arena_allocator(&ar));
    expr(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
*/
