#include "burrow/burrow.h"

static void source(Alloc *a) {
    // doc: source
    Str src = BURROW_S("package main\n"
                       "import (\n"
                       "\"os\"\n"
                       "\"fmt\"\n"
                       ")\n"
                       "func main() {\n"
                       "if len(os.Args)>1{fmt.Println( \"hi\", os.Args[1] )}\n"
                       "x:=0X1P4\n"
                       "fmt.Println(x)\n"
                       "}\n");
    Error err = BURROW_NO_ERROR;
    Slice out = format_source(a, slice_from_str(a, src), &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("format: %s\n", error_text(err));
        return;
    }
    fmt_printf_v("%s", str_from_bytes(out.p, out.len));
    // doc: end
}

static void fragment(Alloc *a) {
    // doc: fragment
    // Two statements, indented one tab, with a blank line after them.
    Str src = BURROW_S("\tx:=1\n\tif x>0{y:=x*2;_=y}\n\n");
    Error err = BURROW_NO_ERROR;
    Slice out = format_source(a, slice_from_str(a, src), &err);
    if (BURROW_OK(err))
        fmt_printf_v("%q\n", str_from_bytes(out.p, out.len));

    // A syntax error comes back as go/parser reported it.
    out = format_source(a, slice_from_str(a, BURROW_S("x := 1 +")), &err);
    if (BURROW_FAILED(err))
        fmt_printf_v("error: %s\n", error_text(err));
    // doc: end
}

static void node(Alloc *a) {
    // doc: node
    Error err = BURROW_NO_ERROR;
    AstExpr x = parser_parse_expr(a, BURROW_S("(6+2*3)/4"), &err);
    if (BURROW_FAILED(err))
        return;
    StringsBuilder b = STRINGS_BUILDER(a);
    err = format_node(a, strings_builder_as_io_writer(&b), token_new_file_set(a),
                      BURROW_ANY(TYPE_AST_EXPR, &x));
    if (BURROW_OK(err))
        fmt_printf_v("%s\n", strings_builder_string(&b));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    source(arena_allocator(&ar));
    fragment(arena_allocator(&ar));
    node(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
*/
