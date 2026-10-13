#include "burrow/burrow.h"

// doc: build
static AstIdent *ident(Alloc *a, TokenFile *f, Int off, Str name) {
    AstIdent *id = ast_new_ident(a, name);
    if (id != NULL)
        id->name_pos = token_file_pos(f, off);
    return id;
}

static void build(Alloc *a) {
    // The tree go/parser would make of this.
    Str src = BURROW_S("package p\n\nvar x = 1\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("p.go"), -1, src.len);
    token_file_set_lines_for_content(f, slice_from_str(a, src));

    AstBasicLit *one = (AstBasicLit *)ast_node_new(a, AST_KIND_BASIC_LIT);
    AstValueSpec *spec = (AstValueSpec *)ast_node_new(a, AST_KIND_VALUE_SPEC);
    AstGenDecl *decl = (AstGenDecl *)ast_node_new(a, AST_KIND_GEN_DECL);
    AstFile *file = (AstFile *)ast_node_new(a, AST_KIND_FILE);
    if (one == NULL || spec == NULL || decl == NULL || file == NULL)
        return;
    one->value_pos = token_file_pos(f, 19);
    one->kind = TOKEN_INT;
    one->value = BURROW_S("1");

    AstIdent *x = ident(a, f, 15, BURROW_S("x"));
    spec->names = slice_append(a, slice_nil(TYPE_AST_IDENT_PTR), &x, 1);
    spec->values = slice_append(a, slice_nil(TYPE_AST_EXPR), &one, 1);
    decl->tok_pos = token_file_pos(f, 11);
    decl->tok = TOKEN_VAR;
    decl->specs = slice_append(a, slice_nil(TYPE_AST_SPEC), &spec, 1);
    file->package = token_file_pos(f, 0);
    file->name = ident(a, f, 8, BURROW_S("p"));
    file->decls = slice_append(a, slice_nil(TYPE_AST_DECL), &decl, 1);

    Error err = ast_print(a, fset, BURROW_ANY(TYPE_AST_FILE_PTR, &file));
    if (!BURROW_OK(err))
        fmt_println_v("print:", err);
}
// doc: end

// doc: inspect
static bool show(void *env, AstNode n) {
    Alloc *a = env;
    if (n == NULL)
        return false;
    switch ((int)n->kind) {
    case AST_KIND_IDENT:
        fmt_println_v("ident", ((AstIdent *)n)->name);
        break;
    case AST_KIND_BASIC_LIT:
        fmt_println_v("literal", ((AstBasicLit *)n)->value);
        break;
    case AST_KIND_BINARY_EXPR:
        fmt_println_v("operator", token_string(((AstBinaryExpr *)n)->op, a));
        break;
    default:
        break;
    }
    return true;
}

static void inspect(Alloc *a) {
    // a + b*2
    AstBinaryExpr *mul = (AstBinaryExpr *)ast_node_new(a, AST_KIND_BINARY_EXPR);
    AstBinaryExpr *add = (AstBinaryExpr *)ast_node_new(a, AST_KIND_BINARY_EXPR);
    AstBasicLit *two = (AstBasicLit *)ast_node_new(a, AST_KIND_BASIC_LIT);
    AstIdent *va = ast_new_ident(a, BURROW_S("a"));
    AstIdent *vb = ast_new_ident(a, BURROW_S("b"));
    if (mul == NULL || add == NULL || two == NULL || va == NULL || vb == NULL)
        return;
    two->kind = TOKEN_INT;
    two->value = BURROW_S("2");
    mul->x = &vb->node;
    mul->op = TOKEN_MUL;
    mul->y = &two->node;
    add->x = &va->node;
    add->op = TOKEN_ADD;
    add->y = &mul->node;

    ast_inspect(&add->node, (AstInspectFunc){show, a});
}
// doc: end

// doc: exports
static void exports(Alloc *a) {
    const char *names[] = {"Open", "close", "Read", "flush"};
    AstFile *file = (AstFile *)ast_node_new(a, AST_KIND_FILE);
    if (file == NULL)
        return;
    file->name = ast_new_ident(a, BURROW_S("p"));
    file->decls = slice_nil(TYPE_AST_DECL);
    for (int i = 0; i < 4; i++) {
        AstFuncDecl *fn = (AstFuncDecl *)ast_node_new(a, AST_KIND_FUNC_DECL);
        AstFuncType *type = (AstFuncType *)ast_node_new(a, AST_KIND_FUNC_TYPE);
        if (fn == NULL || type == NULL)
            return;
        fn->name = ast_new_ident(a, str_from_cstr(names[i]));
        fn->type = type;
        file->decls = slice_append(a, file->decls, &fn, 1);
    }

    bool any = ast_file_exports(file);
    fmt_println_v("exported anything:", any);
    for (Int i = 0; i < file->decls.len; i++) {
        AstFuncDecl *fn = ((AstFuncDecl **)file->decls.p)[i];
        fmt_println_v("func", fn->name->name);
    }
}
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    build(a);
    inspect(a);
    exports(a);
    arena_free(&ar);
    return 0;
}

/* Output:
     0  *ast.File {
     1  .  Package: p.go:1:1
     2  .  Name: *ast.Ident {
     3  .  .  NamePos: p.go:1:9
     4  .  .  Name: "p"
     5  .  }
     6  .  Decls: []ast.Decl (len = 1) {
     7  .  .  0: *ast.GenDecl {
     8  .  .  .  TokPos: p.go:3:1
     9  .  .  .  Tok: var
    10  .  .  .  Lparen: -
    11  .  .  .  Specs: []ast.Spec (len = 1) {
    12  .  .  .  .  0: *ast.ValueSpec {
    13  .  .  .  .  .  Names: []*ast.Ident (len = 1) {
    14  .  .  .  .  .  .  0: *ast.Ident {
    15  .  .  .  .  .  .  .  NamePos: p.go:3:5
    16  .  .  .  .  .  .  .  Name: "x"
    17  .  .  .  .  .  .  }
    18  .  .  .  .  .  }
    19  .  .  .  .  .  Values: []ast.Expr (len = 1) {
    20  .  .  .  .  .  .  0: *ast.BasicLit {
    21  .  .  .  .  .  .  .  ValuePos: p.go:3:9
    22  .  .  .  .  .  .  .  ValueEnd: -
    23  .  .  .  .  .  .  .  Kind: INT
    24  .  .  .  .  .  .  .  Value: "1"
    25  .  .  .  .  .  .  }
    26  .  .  .  .  .  }
    27  .  .  .  .  }
    28  .  .  .  }
    29  .  .  .  Rparen: -
    30  .  .  }
    31  .  }
    32  .  FileStart: -
    33  .  FileEnd: -
    34  .  GoVersion: ""
    35  }
operator +
ident a
operator *
ident b
literal 2
exported anything: true
func Open
func Read
*/
