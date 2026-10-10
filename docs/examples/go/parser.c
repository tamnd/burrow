#include "burrow/burrow.h"

static void parse(Alloc *a) {
    // doc: parse
    Str src = BURROW_S("// Package hello says hello.\n"
                       "package hello\n"
                       "\n"
                       "import (\n"
                       "\t\"fmt\"\n"
                       "\t\"strings\"\n"
                       ")\n"
                       "\n"
                       "// Greet says hello to name.\n"
                       "func Greet(name string) string {\n"
                       "\treturn fmt.Sprint(\"hello, \", strings.TrimSpace(name))\n"
                       "}\n"
                       "\n"
                       "func shout(s string) string { return strings.ToUpper(s) }\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, BURROW_S("hello.go"), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS, &err);
    if (!BURROW_OK(err)) {
        fmt_println_v(error_text(err));
        return;
    }
    fmt_printf_v("package %s\n", f->name->name);
    for (Int i = 0; i < f->imports.len; i++)
        fmt_printf_v("import %s\n", ((AstImportSpec **)f->imports.p)[i]->path->value);
    for (Int i = 0; i < f->decls.len; i++) {
        AstDecl d = ((AstDecl *)f->decls.p)[i];
        if (d->kind != AST_KIND_FUNC_DECL)
            continue;
        AstFuncDecl *fn = (AstFuncDecl *)d;
        TokenPosition pos = token_file_set_position(fset, ast_node_pos(d));
        fmt_printf_v("%s: func %s, doc %q\n", token_position_string(pos, a),
                     fn->name->name, ast_comment_group_text(fn->doc, a));
    }
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
    Str src = BURROW_S("package p\n"
                       "func f() {\n"
                       "\tx := [1, 2]\n"
                       "\tif x { return }\n"
                       "\ty := 3 +\n"
                       "}\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, BURROW_S("bad.go"), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_ALL_ERRORS, &err);
    const GoScannerErrorList *list =
        (const GoScannerErrorList *)errors_as(err, TYPE_GO_SCANNER_ERROR_LIST);
    for (Int i = 0; list != NULL && i < go_scanner_error_list_len(*list); i++) {
        GoScannerError *e = go_scanner_error_list_at(*list, i);
        fmt_printf_v("%s: %s\n", token_position_string(e->pos, a), e->msg);
    }
    if (f != NULL)
        fmt_printf_v("still got %d declaration(s)\n", f->decls.len);
    // doc: end
}

static void expr(Alloc *a) {
    // doc: expr
    Error err = BURROW_NO_ERROR;
    AstExpr x = parser_parse_expr(a, BURROW_S("a + b*c"), &err);
    if (BURROW_OK(err) && x->kind == AST_KIND_BINARY_EXPR) {
        AstBinaryExpr *sum = (AstBinaryExpr *)x;
        fmt_printf_v("top: %s\n", token_string(sum->op, a));
        if (sum->y->kind == AST_KIND_BINARY_EXPR)
            fmt_printf_v("right: %s\n", token_string(((AstBinaryExpr *)sum->y)->op, a));
    }
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    parse(arena_allocator(&ar));
    errors(arena_allocator(&ar));
    expr(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
*/
