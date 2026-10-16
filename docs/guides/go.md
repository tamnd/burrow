# Go source

`burrow/go/token.h` is Go's `go/token`, the bottom layer of Go's own tools for reading Go source. It has the tokens of the language and the positions that tie them back to a file, a line and a column. `burrow/go/scanner.h` is Go's `go/scanner`, which turns source into those tokens. `burrow/go/version.h`, Go's `go/version`, compares Go versions, and `burrow/go/constant.h`, Go's `go/constant`, does exact arithmetic on Go constants. `burrow/go/ast.h` and `burrow/go/parser.h`, Go's `go/ast` and `go/parser`, sit on top of the two and turn source into syntax trees, and `burrow/go/doc/comment.h`, Go's `go/doc/comment`, reads and prints doc comments. `burrow/go/types.h` is Go's `go/types`, the type checker, with the types, objects, scopes and packages it works on. `burrow/go/importer.h`, Go's `go/importer`, gives the type checker the packages a file imports.

## Positions

A position in Go's tools is a `TokenPos`, a single integer. A `TokenFileSet` hands out a range of them to each file you add, so one number says both which file and where in it. The set turns it back into a `TokenPosition`, with a file name, a byte offset, a line and a column:

<!-- example: ../examples/go/token.c#positions -->
```c
Str src = BURROW_S("package main\n\nfunc main() {\n\tprintln(\"hi\")\n}\n");
TokenFileSet *fset = token_new_file_set(a);
TokenFile *f = token_file_set_add_file(fset, BURROW_S("main.go"), -1, src.len);
token_file_set_lines_for_content(f, slice_from_str(a, src));

TokenPos p = token_file_pos(f, strings_index(src, BURROW_S("println")));
TokenPosition pos = token_file_set_position(fset, p);
fmt_println_v(p, token_position_string(pos, a));

TokenFile *g = token_file_set_add_file(fset, BURROW_S("util.go"), -1, 100);
fmt_println_v(token_file_string(f, a), token_file_string(g, a));
fmt_println_v(token_file_name(token_file_set_file(fset, token_file_pos(g, 7))));
token_file_set_free(fset);
```

That prints:

```
30 main.go:4:2
main.go(1-46) util.go(47-147)
util.go
```

`token_file_set_add_file` takes a base and a size. A base of -1 means the next free one, and the set leaves a gap of one after each file, because the end of a file has a position too. A file knows nothing about lines until you tell it. `token_file_set_lines_for_content` finds them in the source, `token_file_add_line` adds them one at a time as a scanner meets them, and `token_file_set_lines` takes a whole table. Until then, everything is on line 1.

The set makes the files and frees them with itself, so a `TokenFile` pointer stays good for as long as the set does. That includes a file taken out with `token_file_set_remove_file`, which stops finding positions but still answers questions about itself. A file added to a second set with `token_file_set_add_existing_files` still belongs to the set that made it.

A set has a lock, and one set can be shared between threads, as in Go. `token_file_set_iterate` lets go of the lock while your function runs, so the function can add and remove files as it goes.

## Line directives

Generated Go code often carries `//line` comments that point back at the file it was generated from. `token_file_add_line_info` records one: from an offset on, positions are reported as if they came from another file and line. `token_file_set_position` follows these, and `token_file_set_position_for` with `adjusted` false gives the place in the file itself:

<!-- example: ../examples/go/token.c#directives -->
```c
Str src = BURROW_S("a\nb\nc\nd\n");
TokenFileSet *fset = token_new_file_set(a);
TokenFile *f = token_file_set_add_file(fset, BURROW_S("gen.go"), -1, src.len);
token_file_set_lines_for_content(f, slice_from_str(a, src));

// From line 3 on, positions are reported as lines of parser.y from 40.
token_file_add_line_info(f, 4, BURROW_S("parser.y"), 40);

TokenPos p = token_file_line_start(f, 4);
fmt_println_v(token_position_string(token_file_set_position(fset, p), a));
fmt_println_v(
    token_position_string(token_file_set_position_for(fset, p, false), a));
token_file_set_free(fset);
```

That prints:

```
parser.y:41:1
gen.go:4:1
```

## Tokens

A `Token` is an integer with Go's values, so `TOKEN_FUNC` is the same number here and in Go. `token_lookup` tells a keyword from an identifier, `token_string` gives a token's text, and `token_precedence` gives a binary operator's precedence, which is what a parser needs to build expressions:

<!-- example: ../examples/go/token.c#tokens -->
```c
Str words[] = {BURROW_S("func"), BURROW_S("Func"), BURROW_S("x1"), BURROW_S("1x")};
for (int i = 0; i < 4; i++) {
    Token tok = token_lookup(words[i]);
    fmt_printf_v("%-5s %-6s identifier=%t exported=%t\n", words[i],
                 token_string(tok, a), token_is_identifier(words[i]),
                 token_is_exported(words[i]));
}
Token ops[] = {TOKEN_LOR, TOKEN_EQL, TOKEN_ADD, TOKEN_MUL, TOKEN_ARROW};
for (int i = 0; i < 5; i++)
    fmt_printf_v("%s binds at %d\n", token_string(ops[i], a),
                 token_precedence(ops[i]));
```

That prints:

```
func  func   identifier=false exported=false
Func  IDENT  identifier=true exported=true
x1    IDENT  identifier=true exported=false
1x    IDENT  identifier=false exported=false
|| binds at 1
== binds at 3
+ binds at 4
* binds at 5
<- binds at 0
```

`token_string` always allocates, so that a value with no name can come back as `token(N)` the way Go's does. `token_is_keyword` asks about a `Token`, and `token_is_keyword_str` asks the same about a string, which is Go's `token.IsKeyword`.

## Saving a file set

`token_file_set_write` and `token_file_set_read` save and restore a set the way Go's `Write` and `Read` do. They take a function that encodes or decodes an `Any`, so the format is up to you. The value has descriptors with Go's field names, so `encoding/gob` can take it as it is, the way the tests use it. `token_file_set_read` copies what the function decoded, so memory the decoder allocated is still yours to free.

## Scanning

A `GoScanner` turns the source of one `TokenFile` into tokens, one per call to `go_scanner_scan`, with each token's position and its literal text. The file has to be the same size as the source, and the scanner adds the file's lines to it as it meets them, so positions come out with lines and columns without a call to `token_file_set_lines_for_content`:

<!-- example: ../examples/go/scanner.c#scan -->
```c
Str src = BURROW_S("cos(x) + 1i*sin(x) // Euler");
TokenFileSet *fset = token_new_file_set(a);
TokenFile *file = token_file_set_add_file(fset, BURROW_S(""), -1, src.len);
GoScanner s;
go_scanner_init(&s, a, file, slice_from_str(a, src), (GoScannerErrorHandler){0},
                GO_SCANNER_SCAN_COMMENTS);
for (;;) {
    Token tok;
    Str lit;
    TokenPos pos = go_scanner_scan(&s, &tok, &lit);
    if (tok == TOKEN_EOF)
        break;
    Str where = token_position_string(token_file_set_position(fset, pos), a);
    fmt_printf_v("%s\t%s\t%q\n", where, token_string(tok, a), lit);
}
token_file_set_free(fset);
```

That prints:

```
1:1	IDENT	"cos"
1:4	(	""
1:5	IDENT	"x"
1:6	)	""
1:8	+	""
1:10	IMAG	"1i"
1:12	*	""
1:13	IDENT	"sin"
1:16	(	""
1:17	IDENT	"x"
1:18	)	""
1:20	COMMENT	"// Euler"
1:28	;	"\n"
```

The scanner puts in a semicolon at the end of a line where Go's rules want one, and at the end of the file, which is the `;` with a literal of `"\n"` at the end. Comments only come out as tokens with `GO_SCANNER_SCAN_COMMENTS`; without it they are skipped. A literal is a view of the source, so the source has to outlive the literals. The one exception is a comment or a raw string with a carriage return in it, which comes back without the carriage returns, copied into the allocator given to `go_scanner_init`.

## Errors

The scanner doesn't stop at a syntax error. It calls the error handler, if there is one, counts the error in `error_count`, and carries on with the best token it can make. The usual handler adds each error to a `GoScannerErrorList`:

<!-- example: ../examples/go/scanner.c#handler -->
```c
typedef struct Collect {
    GoScannerErrorList list;
    Alloc *a;
} Collect;

static void collect(void *env, TokenPosition pos, Str msg) {
    Collect *c = env;
    go_scanner_error_list_add(&c->list, c->a, pos, msg);
}
```

<!-- example: ../examples/go/scanner.c#errors -->
```c
Str src = BURROW_S("z := \"open\ny := 0x\nx := 'ab'\n");
TokenFileSet *fset = token_new_file_set(a);
TokenFile *file = token_file_set_add_file(fset, BURROW_S("bad.go"), -1, src.len);
Collect c = {{0}, a};
GoScanner s;
go_scanner_init(&s, a, file, slice_from_str(a, src),
                BURROW_FN(GoScannerErrorHandler, collect, &c), 0);
Token tok;
do
    go_scanner_scan(&s, &tok, NULL);
while (tok != TOKEN_EOF);

go_scanner_error_list_sort(c.list);
Error err = go_scanner_error_list_err(c.list);
fmt_println_v(err);
BytesBuffer out = BYTES_BUFFER(a);
go_scanner_print_error(bytes_buffer_as_io_writer(&out), err);
fmt_printf_v("%s", bytes_buffer_string(&out, a));
token_file_set_free(fset);
```

That prints:

```
bad.go:1:6: string literal not terminated (and 2 more errors)
bad.go:1:6: string literal not terminated
bad.go:2:8: hexadecimal literal has no digits
bad.go:3:6: illegal rune literal
```

The message the handler gets is only good for the length of the call, and `go_scanner_error_list_add` makes its own copy. `go_scanner_error_list_err` turns the list into an `Error` that shares the list's memory, `errors_as` with `TYPE_GO_SCANNER_ERROR_LIST` gets the list back out of it, and `go_scanner_print_error` writes one error per line. `go_scanner_error_list_remove_multiples` keeps only the first error on each line, which is what Go's tools do before they report.

## Versions

`burrow/go/version.h` is Go's `go/version`. It compares Go versions written the way the toolchain names them, like `go1.21`, `go1.21.0`, `go1.22rc2` and `go1.23.4-custom`:

<!-- example: ../examples/go/version.c#version -->
```c
Str pairs[][2] = {
    {BURROW_S("go1.21"), BURROW_S("go1.21rc1")},
    {BURROW_S("go1.21rc1"), BURROW_S("go1.21.0")},
    {BURROW_S("go1.9"), BURROW_S("go1.10")},
    {BURROW_S("go1.20"), BURROW_S("go1.20.0")},
};
for (int i = 0; i < 4; i++)
    fmt_println_v(pairs[i][0], pairs[i][1],
                  version_compare(pairs[i][0], pairs[i][1]));

Str vs[] = {BURROW_S("go1.22.3"), BURROW_S("go1.23rc1"), BURROW_S("go1"),
            BURROW_S("1.22")};
for (int i = 0; i < 4; i++)
    fmt_printf_v("%-10s valid=%v lang=%q\n", vs[i], version_is_valid(vs[i]),
                 version_lang(a, vs[i]));
```

That prints:

```
go1.21 go1.21rc1 -1
go1.21rc1 go1.21.0 -1
go1.9 go1.10 -1
go1.20 go1.20.0 0
go1.22.3   valid=true lang="go1.22"
go1.23rc1  valid=true lang="go1.23"
go1        valid=true lang="go1"
1.22       valid=false lang=""
```

The ordering is the toolchain's. Since Go 1.21 the language version `go1.21` comes before its release candidates and `go1.21.0`, while for older versions `go1.20` and `go1.20.0` are the same thing. A version needs the `go` prefix, and anything after a `-` is ignored. `version_compare` puts an invalid version below every valid one, and `version_lang` returns an empty string for it. `version_lang` usually returns the front of the string you pass in. The exception is a bare major version, so `go222` gives `go222.0`, and that one is built in the allocator.

## Constants

`burrow/go/constant.h` is Go's `go/constant`. A `ConstantValue` is the exact value of an untyped Go constant, the way a compiler sees it before the constant is given a type: a bool, a string, an integer of any size, a fraction or a float of very high precision, or a complex number made of two of those. Values come from literals with `constant_make_from_literal` or from the `constant_make_*` constructors, and the operations make new ones. Here is (2.3 + 5i) * 11 worked out exactly, and then turned into doubles:

<!-- example: ../examples/go/constant.c#arith -->
```c
// (2.3 + 5i) * 11, worked out exactly.
ConstantValue ar = constant_make_float64(a, 2.3);
ConstantValue ai = constant_make_imag(a, constant_make_int64(5));
ConstantValue x = constant_binary_op(a, ar, TOKEN_ADD, ai);
ConstantValue c = constant_binary_op(a, x, TOKEN_MUL, constant_make_uint64(a, 11));

bool exact;
double re = constant_float64_val(constant_real(c), &exact);
if (!exact)
    fmt_printf_v("real part %s is not exact as a double\n",
                 constant_value_string(constant_real(c), a));
double im = constant_float64_val(constant_imag(c), &exact);
fmt_println_v("go/constant", BURROW_ANY(TYPE_CONSTANT_VALUE, &c));
fmt_println_v("double", re, im, exact);

// 11 / 0.5
ConstantValue q = constant_binary_op(a, constant_make_uint64(a, 11), TOKEN_QUO,
                                     constant_make_float64(a, 0.5));
fmt_println_v(constant_value_string(q, a));
```

That prints:

```
real part 25.3 is not exact as a double
go/constant (25.3 + 55i)
double 25.299999999999997 55 true
22
```

The kinds go up from `CONSTANT_INT` to `CONSTANT_FLOAT` to `CONSTANT_COMPLEX` as needed, so an Int divided by a Float is a Float. `TOKEN_QUO` on two Ints gives the exact fraction, and `TOKEN_QUO_ASSIGN` gives the truncated integer quotient. `constant_value_string` rounds a number that is not an integer to 6 digits and `constant_value_exact_string` writes it in full, and the type descriptor `TYPE_CONSTANT_VALUE` has the short form as its `String` method, so a value can go straight to `fmt`.

`constant_unary_op` takes a precision, which makes `^` work on an unsigned integer of that many bits:

<!-- example: ../examples/go/constant.c#unary -->
```c
ConstantValue vs[] = {
    constant_make_bool(true),
    constant_make_float64(a, 2.7),
    constant_make_uint64(a, 42),
};
for (int i = 0; i < 3; i++) {
    switch (constant_value_kind(vs[i])) {
    case CONSTANT_BOOL:
        vs[i] = constant_unary_op(a, TOKEN_NOT, vs[i], 0);
        break;
    case CONSTANT_FLOAT:
        vs[i] = constant_unary_op(a, TOKEN_SUB, vs[i], 0);
        break;
    case CONSTANT_INT:
        // 16 bits of precision, the same as ^uint16(v).
        vs[i] = constant_unary_op(a, TOKEN_XOR, vs[i], 16);
        break;
    default:
        break;
    }
}
for (int i = 0; i < 3; i++)
    fmt_println_v(constant_value_string(vs[i], a));
```

That prints:

```
false
-2.7
65493
```

`constant_compare` works for strings as well as numbers, so it can sort:

<!-- example: ../examples/go/constant.c#compare -->
```c
static int by_value(void *env, const void *x, const void *y) {
    (void)env;
    const ConstantValue *p = x, *q = y;
    if (constant_compare(*p, TOKEN_LSS, *q))
        return -1;
    if (constant_compare(*p, TOKEN_GTR, *q))
        return +1;
    return 0;
}

static void compare(Alloc *a) {
    ConstantValue vs[] = {
        constant_make_string(a, BURROW_S("Z")),
        constant_make_string(a, BURROW_S("bacon")),
        constant_make_string(a, BURROW_S("go")),
        constant_make_string(a, BURROW_S("Frame")),
        constant_make_string(a, BURROW_S("defer")),
        constant_make_from_literal(a, BURROW_S("\"a\""), TOKEN_STRING, 0),
    };
    Slice s = slice_from(vs, 6, 6, TYPE_CONSTANT_VALUE);
    slices_sort_func(s, BURROW_FN(SlicesCmpFunc, by_value, NULL));
    for (int i = 0; i < 6; i++)
        fmt_println_v(constant_string_val(vs[i]));
}
```

That prints:

```
Frame
Z
a
bacon
defer
go
```

`constant_sign` is -1, 0 or +1, and a complex value is 0 only when both its parts are:

<!-- example: ../examples/go/constant.c#sign -->
```c
static ConstantValue mk_complex(Alloc *a, ConstantValue re, ConstantValue im) {
    return constant_binary_op(a, re, TOKEN_ADD, constant_make_imag(a, im));
}

static void sign(Alloc *a) {
    ConstantValue zero = constant_make_int64(0);
    ConstantValue one = constant_make_int64(1);
    ConstantValue neg_one = constant_make_int64(-1);
    ConstantValue vs[] = {
        neg_one,
        mk_complex(a, zero, neg_one),
        mk_complex(a, one, neg_one),
        mk_complex(a, neg_one, one),
        mk_complex(a, neg_one, neg_one),
        zero,
        mk_complex(a, zero, zero),
        one,
        mk_complex(a, zero, one),
        mk_complex(a, one, one),
    };
    for (int i = 0; i < 10; i++)
        fmt_printf_v("% d %s\n", constant_sign(vs[i]), constant_value_string(vs[i], a));
}
```

That prints:

```
-1 -1
-1 (0 + -1i)
-1 (1 + -1i)
-1 (-1 + 1i)
-1 (-1 + -1i)
 0 0
 0 (0 + 0i)
 1 1
 1 (0 + 1i)
 1 (1 + 1i)
```

`constant_val` gives a value back as an `Any`: a `bool`, a `Str` or an `int64_t` when it fits one, and otherwise the `BigInt`, `BigRat` or `BigFloat` it is held in. A float that came from a double is held as a fraction:

<!-- example: ../examples/go/constant.c#val -->
```c
ConstantValue vs[] = {
    constant_make_int64(INT64_MAX),
    constant_make_float64(a, MATH_E),
    constant_make_bool(true),
    constant_make(a, BURROW_ANY_VAL(TYPE_BOOL, bool, false)),
};
for (int i = 0; i < 4; i++)
    fmt_printf_v("%v\n", constant_val(a, vs[i]));
```

That prints:

```
9223372036854775807
6121026514868073/2251799813685248
true
false
```

A ConstantValue is a small struct passed by value, and `{0}` is the unknown value, which is what an operation on an unknown value or a bad literal gives back. Anything bigger than an `int64_t` lives in the allocator passed to the function that made it, and values share that memory, since none of them ever changes. Nothing frees a single value, so make them in an arena and free the arena when you are done. Operations panic on operands that make no sense, like `!1` or `true + true`, with the same messages as Go.

## Syntax trees

`burrow/go/ast.h` is Go's `go/ast`, the syntax tree of a Go source file. Every node starts with an `AstBase` header that holds its kind, and an `AstNode` is a pointer to that header, so the `AstExpr`, `AstStmt`, `AstDecl` and `AstSpec` interfaces are all `AstNode` and a switch on `n->kind` tells them apart. `ast_node_new` makes a zeroed node of a kind in an allocator. `parser_parse_file`, under Parsing below, makes trees from source, and they can also be built by hand, as here. `ast_print` writes one out the way `ast.Print` does, with positions from a file set:

<!-- example: ../examples/go/ast.c#build -->
```c
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
```

That prints:

```
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
```

`ast_inspect` walks a tree depth first. It calls the function for each node and goes into the node's children while the function returns true, then calls it once more with NULL when it is done with them:

<!-- example: ../examples/go/ast.c#inspect -->
```c
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
```

That prints:

```
operator +
ident a
operator *
ident b
literal 2
```

`ast_walk` is the same walk with an `AstVisitor`, and `ast_preorder` gives the nodes as an iterator. `ast_file_exports` cuts a file down to its exported declarations and says whether anything is left:

<!-- example: ../examples/go/ast.c#exports -->
```c
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
```

That prints:

```
exported anything: true
func Open
func Read
```

Nodes and the slices in them live in the allocator they were made in, and nothing frees a single node, so build a tree in an arena and free the arena when you are done with it. The tree functions that build new lists, like `ast_merge_package_files`, `ast_sort_imports` and `ast_new_comment_map`, take the allocator to use as their first argument. `ast_merge_package_files` follows Go 1.27, which fixed the merged file's `FileStart` that Go 1.26 always left at 0.

## Build constraints

`burrow/go/build/constraint.h` is Go's `go/build/constraint`. `constraint_parse` takes one `//go:build` or `// +build` line and gives back the expression in it as a tree of tag, not, and and or nodes. `constraint_expr_eval` decides whether a build matches by asking a function about each tag, and `constraint_go_version` works out the oldest Go the line allows:

<!-- example: ../examples/go/constraint.c#parse -->
```c
Str lines[] = {
    BURROW_S("//go:build linux && (amd64 || arm64)"),
    BURROW_S("//go:build !windows && go1.21"),
    BURROW_S("// +build darwin,!cgo freebsd"),
    BURROW_S("//go:build linux &&"),
    BURROW_S("// just a comment"),
};
ConstraintTagFunc ok = BURROW_FN(ConstraintTagFunc, linux_amd64, NULL);
for (int i = 0; i < 5; i++) {
    Error err = BURROW_NO_ERROR;
    ConstraintExpr x = constraint_parse(a, lines[i], &err);
    if (!BURROW_OK(err)) {
        fmt_printf_v("%q: %s\n", lines[i], error_text(err));
        continue;
    }
    fmt_printf_v("%s  build=%v min=%q\n", constraint_expr_string(x, a),
                 constraint_expr_eval(x, ok), constraint_go_version(a, x));
}
```

That prints:

```
linux && (amd64 || arm64)  build=true min=""
!windows && go1.21  build=true min="go1.21"
(darwin && !cgo) || freebsd  build=false min=""
"//go:build linux &&": unexpected end of expression
"// just a comment": not a build constraint
```

The old `// +build` form is read the way the go command reads it, with spaces meaning or and commas meaning and. A line that is neither form gives the "not a build constraint" error, and a bad `//go:build` expression gives a `ConstraintSyntaxError` with the byte offset of the problem, which `errors_as` with `TYPE_CONSTRAINT_SYNTAX_ERROR` gets out of the error. Both forms have a size limit, so a huge line fails instead of using up the stack.

`constraint_plus_build_lines` goes the other way and writes `// +build` lines that mean the same as an expression, for code that still has to build with Go 1.16 and older:

<!-- example: ../examples/go/constraint.c#plusbuild -->
```c
Error err = BURROW_NO_ERROR;
ConstraintExpr x =
    constraint_parse(a, BURROW_S("//go:build !(windows || plan9) && cgo"), &err);
Slice lines = slice_nil(TYPE_STRING);
if (BURROW_OK(err))
    lines = constraint_plus_build_lines(a, x, &err);
if (!BURROW_OK(err))
    fmt_println_v(error_text(err));
for (Int i = 0; i < lines.len; i++)
    fmt_println_v(((Str *)lines.p)[i]);
```

That prints:

```
// +build !windows,!plan9,cgo
```

Not every expression fits. The `// +build` form can only say an and of ors of ands, and when the expression needs more than that the call returns an error and no lines. The tree, its tags and the lines all live in the allocator you pass in.

## Parsing

`burrow/go/parser.h` is Go's `go/parser`. `parser_parse_file` reads the source of one Go file and gives back its `AstFile`, with every position recorded in the file set you pass. The source can be a `Str`, a byte slice, a `BytesBuffer` or an `IoReader`, or the nil `Any` to read the file named by the filename. `PARSER_PARSE_COMMENTS` keeps the comments, which is what fills in the doc comment of each declaration:

<!-- example: ../examples/go/parser.c#parse -->
```c
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
```

That prints:

```
package hello
import "fmt"
import "strings"
hello.go:10:1: func Greet, doc "Greet says hello to name.\n"
hello.go:14:1: func shout, doc ""
```

A syntax error does not lose the tree. The parser puts `BadExpr`, `BadStmt` and `BadDecl` nodes where it could not make sense of the source, carries on, and returns the file along with a `GoScannerErrorList` of everything it found, sorted by position:

<!-- example: ../examples/go/parser.c#errors -->
```c
Str src = BURROW_S("package p\n"
                   "\n"
                   "func f() {\n"
                   "\tx := 1 +\n"
                   "}\n"
                   "\n"
                   "func g(a int {\n"
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
```

That prints the same errors Go's parser reports for this source, the repeated one included:

```
bad.go:5:1: expected operand, found '}'
bad.go:8:3: expected ';', found 'EOF'
bad.go:8:3: expected ';', found 'EOF'
bad.go:8:3: expected '}', found 'EOF'
still got 1 declaration(s)
```

Without `PARSER_ALL_ERRORS` the parser keeps the report short: it drops an error on the same line as the one before it, and it gives up once it has more than ten. The list lives in the allocator along with the tree, so use `error_retain` on the error if it has to outlive the arena.

`parser_parse_expr` parses a single expression, which is handy for small tools and tests:

<!-- example: ../examples/go/parser.c#expr -->
```c
Error err = BURROW_NO_ERROR;
AstExpr x = parser_parse_expr(a, BURROW_S("a + b*c"), &err);
if (BURROW_OK(err) && x->kind == AST_KIND_BINARY_EXPR) {
    AstBinaryExpr *sum = (AstBinaryExpr *)x;
    fmt_printf_v("top: %s\n", token_string(sum->op, a));
    if (sum->y->kind == AST_KIND_BINARY_EXPR)
        fmt_printf_v("right: %s\n", token_string(((AstBinaryExpr *)sum->y)->op, a));
}
```

That prints:

```
top: +
right: *
```

`parser_parse_dir` is here too, for code that still uses it, though Go deprecates it because it knows nothing of build tags. Everything the parser makes is in the allocator you pass, so parse into an arena and free it in one go. Go stops at 100000 levels of nesting. A C stack cannot grow the way a goroutine's does, so the parser also stops with the same "exceeded max nesting depth" error when the stack is about to run out. To parse code nested thousands deep, run the parse on a goroutine started with `go_stack` and a bigger stack.

## Doc comments

`burrow/go/doc/comment.h` is Go's `go/doc/comment`, which reads the text of a doc comment, with the comment markers already gone, and prints it back out as a comment, as HTML, as Markdown or as plain text. `comment_parser_parse` turns the text into a `CommentDoc`, a list of blocks. Like the nodes of `go/ast`, every block and every piece of text starts with a `CommentBase` header that holds its kind, so a switch on `b->kind` tells a heading from a paragraph, a list or a code block:

<!-- example: ../examples/go/doc_comment.c#parse -->
```c
Str text = BURROW_S("Package hello says hello. See [strings.TrimSpace].\n"
                    "\n"
                    "# Usage\n"
                    "\n"
                    "Call it like this:\n"
                    "\n"
                    "\tfmt.Println(hello.Greet(\"world\"))\n"
                    "\n"
                    "It knows two greetings:\n"
                    "  - hello\n"
                    "  - goodbye\n");
CommentDoc *d = comment_parser_parse(NULL, a, text);
for (Int i = 0; i < d->content.len; i++) {
    CommentBlock b = ((CommentBlock *)d->content.p)[i];
    switch ((int)b->kind) {
    case COMMENT_KIND_PARAGRAPH:
        fmt_printf_v("paragraph of %d text(s)\n",
                     ((CommentParagraph *)b)->text.len);
        break;
    case COMMENT_KIND_HEADING:
        fmt_printf_v("heading, id %s\n",
                     comment_heading_default_id((CommentHeading *)b, a));
        break;
    case COMMENT_KIND_CODE:
        fmt_printf_v("code %q\n", ((CommentCode *)b)->text);
        break;
    case COMMENT_KIND_LIST:
        fmt_printf_v("list of %d item(s)\n", ((CommentList *)b)->items.len);
        break;
    default:
        break;
    }
}
```

That prints:

```
paragraph of 3 text(s)
heading, id hdr-Usage
paragraph of 1 text(s)
code "fmt.Println(hello.Greet(\"world\"))\n"
paragraph of 1 text(s)
list of 2 item(s)
```

A NULL parser is Go's zero `Parser`. Set `words` to a map of words that should come out in italics or as links, and `lookup_package` and `lookup_sym` to say which names in square brackets are doc links. Without `lookup_package`, the single element packages of the standard library, such as `[strings]`, still resolve. The doc and everything in it lives in the allocator you pass, text included, so the string you parsed can go away.

A `CommentPrinter` holds the settings of the four printers, and its zero value prints the way Go's does. `text_width` and `text_prefix` shape the text output, and `doc_link_base_url` turns doc links into links in HTML and Markdown:

<!-- example: ../examples/go/doc_comment.c#print -->
```c
Str text = BURROW_S("Greet returns a greeting for name, as in\n"
                    "\"hello, name\". It trims the name with [strings.TrimSpace]\n"
                    "first, and an empty name gets a greeting all the same.\n");
CommentDoc *d = comment_parser_parse(NULL, a, text);
CommentPrinter p = {0};
p.doc_link_base_url = BURROW_S("https://pkg.go.dev");
p.text_width = 40;
Slice out = comment_printer_text(&p, a, d);
fmt_printf_v("-- text\n%s", str_from_bytes(out.p, out.len));
out = comment_printer_markdown(&p, a, d);
fmt_printf_v("-- markdown\n%s", str_from_bytes(out.p, out.len));
out = comment_printer_html(&p, a, d);
fmt_printf_v("-- html\n%s", str_from_bytes(out.p, out.len));
```

That prints:

```
-- text
Greet returns a greeting for name,
as in "hello, name". It trims the name
with strings.TrimSpace first, and an
empty name gets a greeting all the same.
-- markdown
Greet returns a greeting for name, as in "hello, name". It trims the name with [strings.TrimSpace](https://pkg.go.dev/strings#TrimSpace) first, and an empty name gets a greeting all the same.
-- html
<p>Greet returns a greeting for name, as in
&quot;hello, name&quot;. It trims the name with <a href="https://pkg.go.dev/strings#TrimSpace">strings.TrimSpace</a>
first, and an empty name gets a greeting all the same.
```

Each printer returns a new byte slice in the allocator. Set `heading_id` or `doc_link_url` to choose the heading anchors and the doc link targets yourself; otherwise `comment_heading_default_id` and `comment_doc_link_default_url` pick them, as Go's `DefaultID` and `DefaultURL` do.

## Printing

`burrow/go/printer.h` is Go's `go/printer`, which writes a syntax tree back out as Go source. `printer_fprint` takes the file set the tree's positions come from and the node as an `Any` that points at the node pointer, the same way Go's takes an `any`:

<!-- example: ../examples/go/printer.c#fprint -->
```c
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
```

The layout is gofmt's, with comments kept in their places:

```
package main

import "fmt"

func main() {
	fmt.Println("hello", 1+2)	// say hello
}
```

`printer_fprint` is `printer_config_fprint` with a tab width of 8 and no mode flags, so it pads columns with tabs. gofmt indents with tabs but aligns with spaces, which takes `PRINTER_USE_SPACES` and `PRINTER_TAB_INDENT`:

<!-- example: ../examples/go/printer.c#config -->
```c
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
```

That prints:

```
package p

type Person struct {
	Name string // full name
	Age  int    // in years
}
```

The node can also be a single expression, statement, declaration or spec, a `Slice` of statements or declarations (`TYPE_AST_STMT_SLICE` and `TYPE_AST_DECL_SLICE`), or a `PrinterCommentedNode`, which brings the file's comments along with a node that is not a whole file. A tree built by hand, or one whose positions are not in the file set, still prints:

<!-- example: ../examples/go/printer.c#expr -->
```c
Error err = BURROW_NO_ERROR;
AstExpr x = parser_parse_expr(a, BURROW_S("a*(b+c)  ==  f(x,y)[ 0 ]"), &err);
if (BURROW_FAILED(err))
    return;
StringsBuilder b = STRINGS_BUILDER(a);
err = printer_fprint(a, strings_builder_as_io_writer(&b), token_new_file_set(a),
                     BURROW_ANY(TYPE_AST_EXPR, &x));
if (BURROW_OK(err))
    fmt_printf_v("%s\n", strings_builder_string(&b));
```

That prints:

```
a*(b+c) == f(x, y)[0]
```

A node of any other type gives an error, and nothing is written. The printer works in an arena of its own, taken from the allocator you pass and given back before it returns, so the only memory it leaves behind is what the writer kept.

## Formatting

`burrow/go/format.h` is Go's `go/format`, the formatting gofmt does, as a library. `format_source` takes Go source as bytes and gives it back in gofmt's layout, with the imports sorted and number literals in their canonical form:

<!-- example: ../examples/go/format.c#source -->
```c
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
```

That prints:

```
package main

import (
	"fmt"
	"os"
)

func main() {
	if len(os.Args) > 1 {
		fmt.Println("hi", os.Args[1])
	}
	x := 0x1p4
	fmt.Println(x)
}
```

The source does not have to be a whole file. A list of declarations or of statements works too, and comes back with the leading and trailing space it had and its first line's indentation kept, which is what an editor wants when it formats a selection. A syntax error is the `GoScannerErrorList` that go/parser gave back, and the result is a nil slice:

<!-- example: ../examples/go/format.c#fragment -->
```c
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
```

That prints:

```
"\tx := 1\n\tif x > 0 {\n\t\ty := x * 2\n\t\t_ = y\n\t}\n\n"
error: 3:1: expected operand, found '}'
```

The line and column in that error are in the source go/format parsed, which for a statement list has a package clause and a function wrapped around it, so they are Go's numbers and not quite where the fragment's text was.

`format_node` formats a tree you already have, written to an `IoWriter`. It takes the same nodes as `printer_fprint`, and leaves the tree as it was, sorting a copy when a file's imports need it:

<!-- example: ../examples/go/format.c#node -->
```c
Error err = BURROW_NO_ERROR;
AstExpr x = parser_parse_expr(a, BURROW_S("(6+2*3)/4"), &err);
if (BURROW_FAILED(err))
    return;
StringsBuilder b = STRINGS_BUILDER(a);
err = format_node(a, strings_builder_as_io_writer(&b), token_new_file_set(a),
                  BURROW_ANY(TYPE_AST_EXPR, &x));
if (BURROW_OK(err))
    fmt_printf_v("%s\n", strings_builder_string(&b));
```

That prints:

```
(6 + 2*3) / 4
```

`format_source` parses into the allocator you pass and leaves the tree there next to the result, so an arena is the easy choice. Formatting changes now and then between Go versions, and it can here too, so a check that compares output byte for byte should pin the library version.

## Package documentation

`burrow/go/doc.h` is Go's `go/doc`, what `go doc` and pkg.go.dev build their pages from. `doc_new_from_files` takes the parsed files of one package, comments included, and gives back a `DocPackage`: the package comment, then the constants, variables, functions and types worth documenting, sorted and grouped the way `go doc` shows them. A function that returns a type is filed under that type, methods go with their receiver, and the examples in the `_test.go` files are attached to whatever they are named after:

<!-- example: ../examples/go/doc.c#package -->
```c
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
```

That prints:

```
package greet: Package greet says hello.
type Greeter: A Greeter greets people by name.
  func New: New returns a Greeter for name.
  method (*Greeter) Greet: Greet prints a greeting.
    example, output "Hello, world\n"
```

`helper` is not there because it is unexported. `DOC_ALL_DECLS` keeps everything, and `DOC_ALL_METHODS` lists the methods promoted from every embedded field rather than only from the unexported ones. As in Go the files are changed on the way: function bodies are dropped and unexported fields and declarations filtered out, so pass `DOC_PRESERVE_AST` if the trees are needed afterwards. Everything in the `DocPackage` lives in the allocator passed in and points into the trees, so use the arena the files were parsed into.

`doc_synopsis` is the first sentence of a doc comment, as plain text on one line, and an empty string for comments that do not describe anything, such as a copyright notice:

<!-- example: ../examples/go/doc.c#synopsis -->
```c
Str text = BURROW_S("Package sort provides primitives for sorting slices and\n"
                    "user-defined collections. It is fast.\n");
fmt_printf_v("%q\n", doc_synopsis(a, text));
fmt_printf_v("%q\n", doc_synopsis(a, BURROW_S("Copyright 2009 The Go Authors.")));
```

That prints:

```
"Package sort provides primitives for sorting slices and user-defined collections."
""
```

`doc_package_synopsis`, `doc_package_html`, `doc_package_markdown` and `doc_package_text` do the same with doc links resolved against the package, through the `CommentParser` and `CommentPrinter` that `doc_package_parser` and `doc_package_printer` set up. `doc_examples` finds the examples in a set of test files on its own, without the package they belong to.

## Building packages

`burrow/go/build.h` is Go's `go/build`, which decides what makes up a package: which files in a directory are compiled for a given system, what they import, and where an import path lives on disk. A `BuildContext` describes the build. `build_default` fills one in from the environment the way Go does, with `GOOS`, `GOARCH`, `GOROOT`, `GOPATH` and `CGO_ENABLED`, and any field can be changed afterwards. `build_context_import_dir` reads a directory under that context and sorts its files into the lists of a `BuildPackage`:

<!-- example: ../examples/go/build.c#import -->
```c
BuildContext ctxt = build_default(a);
ctxt.goos = BURROW_S("linux");
ctxt.goarch = BURROW_S("amd64");
Error err = BURROW_NO_ERROR;
BuildPackage *p = build_context_import_dir(&ctxt, a, dir, 0, &err);
if (BURROW_FAILED(err)) {
    fmt_printf_v("%s\n", error_text(err));
    return;
}
Str sp = BURROW_S(" ");
fmt_printf_v("package %s: %s\n", p->name, p->doc);
fmt_printf_v("go files: %s\n", strings_join(a, p->go_files, sp));
fmt_printf_v("ignored: %s\n", strings_join(a, p->ignored_go_files, sp));
fmt_printf_v("test files: %s\n", strings_join(a, p->test_go_files, sp));
fmt_printf_v("imports: %s\n", strings_join(a, p->imports, sp));
fmt_printf_v("test imports: %s\n", strings_join(a, p->test_imports, sp));
fmt_printf_v("tags seen: %s\n", strings_join(a, p->all_tags, sp));
```

That prints:

```
package greet: Package greet says hello.
go files: greet.go greet_linux.go
ignored: gen.go greet_windows.go
test files: greet_test.go
imports: fmt os
test imports: testing
tags seen: ignore linux windows
```

`greet_windows.go` is left out by its name and `gen.go` by its `//go:build` line. Both end up in `ignored_go_files`, and the tags they mention are in `all_tags`, so a tool can tell which files could change under another build. `build_context_import` does the same for an import path. It looks in `GOROOT` and `GOPATH`, follows vendor directories, and in module mode asks `go list` through the `go` command in `GOROOT`, as Go does. A missing package still gives back a `BuildPackage` with what was learned along with the error. A directory without Go files gives a `BuildNoGoError`, and one holding two packages gives a `BuildMultiplePackageError`, which `errors_as` finds.

`build_context_match_file` asks the same question about a single file:

<!-- example: ../examples/go/build.c#match -->
```c
BuildContext ctxt = build_default(a);
ctxt.goos = BURROW_S("windows");
ctxt.goarch = BURROW_S("arm64");
const char *names[] = {"greet.go", "greet_linux.go", "greet_windows.go", "gen.go"};
for (int i = 0; i < 4; i++) {
    Error err = BURROW_NO_ERROR;
    Str name = str_from_cstr(names[i]);
    bool ok = build_context_match_file(&ctxt, a, dir, name, &err);
    fmt_printf_v("%s: %v\n", name, ok);
}
```

That prints:

```
greet.go: true
greet_linux.go: false
greet_windows.go: true
gen.go: false
```

The file system is reached through the hooks of the context, `join_path`, `read_dir`, `open_file` and the rest. They are left empty for the real file system, and setting them makes go/build work on files kept anywhere else.

## Types

`burrow/go/types.h` is the model part of Go's `go/types`. It has the types of Go programs, the objects that name them, the scopes and packages that hold the objects, and the universe everything starts from. The checker that fills them in from syntax trees is under [Type checking](#type-checking). Go's `Type` and `Object` interfaces are `TypesType` and `TypesObject` here. `types_type` and `types_object` turn a pointer to a concrete type or object into one, and the `types_as_` functions turn one back, giving `NULL` when it is something else.

Types and objects point at each other in cycles, since a named type's methods have the named type as their receiver. None of them can be freed on their own, so make them all in one arena and free the arena at the end. This builds `type Celsius float64` with a `String` method and prints a few types that use it:

<!-- example: ../examples/go/types.c#model -->
```c
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
```

That prints:

```
map[string][]example.com/temp.Celsius
map[string][]Celsius
func (example.com/temp.Celsius).String() string
float64
comparable: true false
```

The qualifier decides how a package shows up in a name. `TYPES_NO_QUALIFIER` writes the full path, and `types_relative_to` leaves out the package you are in, the same as Go's `RelativeTo`. A map is never comparable, so `types_comparable` is false for it, as it is in Go.

The universe is the scope that holds `int`, `error`, `true`, `len` and the rest of Go's predeclared names. `types_universe` builds it the first time it is asked for, and it is shared by every thread after that:

<!-- example: ../examples/go/types.c#universe -->
```c
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
```

That prints:

```
type error interface{Error() string}
interface{Error() string}
rune == int32: true
untyped float -> float64
```

`types_identical` is Go's `Identical`, and `types_default` gives the type an untyped constant takes when nothing else decides it.

Looking up `x.f` is `types_lookup_selection`. It walks embedded fields the way the compiler does and gives back a `TypesSelection`, which says what was found, the path of field indices to it, and whether a pointer was followed on the way. `types_lookup_field_or_method` does the same lookup and hands the parts back separately, which is how it can also tell you that a name was ambiguous. `types_new_method_set` gives the method set of a type, and `types_missing_method` gives the method of an interface that a type lacks. They all allocate what they return from the allocator you pass, and anything they only need during the walk goes in a scratch arena that is freed before they return.

<!-- example: ../examples/go/types.c#lookup -->
```c
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
```

That prints:

```
field (Outer) N int depth 2
Reset on a value: false on a variable: true
Outer has 0 methods
MethodSet {
	method (*example.com/box.Outer) Reset()
}
Outer lacks Reset wrong type: true
*Outer implements it: true
```

`Reset` has a pointer receiver, so it is in the method set of `*Outer` and not of `Outer`. That is also why `types_missing_method` reports it as the wrong type: `Outer` has the method, just not on a value.

## Sizes

`TypesSizes` is Go's `Sizes` interface, which says how big a type is, how it is aligned and where each field of a struct starts. `TypesStdSizes` is Go's `StdSizes`, a simple layout set by a word size and a largest alignment, and `types_std_sizes_as_sizes` turns one into a `TypesSizes`. `types_sizes_for` is `SizesFor`: give it a compiler and an architecture and it gives back the layout that compiler uses there. It knows gc and gccgo, and for anything else the result has a `NULL` vt, where Go returns nil.

<!-- example: ../examples/go/types.c#sizes -->
```c
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
```

That prints:

```
amd64 struct{a bool; b int64; c bool}
  size 24 align 8 offsets [0 8 16]
amd64 struct{b int64; a bool; c bool}
  size 16 align 8 offsets [0 8 9]
386 struct{a bool; b int64; c bool}
  size 16 align 4 offsets [0 4 12]
386 struct{b int64; a bool; c bool}
  size 12 align 4 offsets [0 8 9]
tinygo known: false
```

On amd64 an `int64` is aligned to 8 bytes, so a `bool` in front of it wastes 7, and putting the `int64` first saves 8 bytes. On 386 the largest alignment is 4, so the same struct is smaller. `types_sizes_offsetsof` allocates its result from the allocator you pass. A size that doesn't fit in an `int64` comes back negative, as it does in Go.

## Expressions in messages

`types_expr_string` is Go's `ExprString`, the form the checker uses to show an expression in an error message. It is Go syntax, made shorter: a function literal keeps only its type, a composite literal with elements gets an ellipsis, and every binary operator has a blank on each side. `types_write_expr` writes the same thing to a `BytesBuffer`.

<!-- example: ../examples/go/types.c#exprstring -->
```c
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
```

That prints:

```
f(a, b...)[i:j]
(func(x int) string literal)
map[string]int{…}
x.(interface{m() int})
-x + y * <-ch
```

Parentheses are written only where the source has them, since the parser keeps them in the tree.

## Hashing types

Two types made separately can be the same type, so a pointer is no good as a key for them. `TypesHasher` is Go's `types.Hasher`: a hash that gives identical types the same value, and an equality that is `types_identical`. `TypesHasherIgnoreTags` is the same with struct tags left out, matching `types_identical_ignore_tags`. Each can be handed to anything that takes a `MaphashHasher` through `types_hasher_as_hasher` or `types_hasher_ignore_tags_as_hasher`, and the values it is given point at a `TypesType`.

In the example, `hash_of` seeds a `MaphashHash`, runs the hasher on one type and returns `maphash_hash_sum64`, and `tagged` makes a `struct{F int}` with the tag it is given. Both are in the example file.

<!-- example: ../examples/go/types.c#hashing -->
```c
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
```

That prints:

```
map[string][]string same pointer: false
same hash: true equal: true
struct{F int "json:\"f\""}
struct{F int "json:\"g\""}
Hasher equal: false
HasherIgnoreTags equal: true same hash: true
```

Types that hash the same are not always identical. Two interfaces with the same number of methods but different type sets hash the same, as they do in Go, so a container still has to call the equality.

## Instantiating generic types

`types_instantiate` is Go's `Instantiate`. It takes a generic named type, alias or function signature and the type arguments, and returns the instance. Instances made through the same `TypesContext` with identical arguments are the same pointer, so a context lets you compare instances with `==`. A NULL context makes a new one for the call.

With `validate` set it checks the number of type arguments and that each one satisfies its constraint. When one does not, the result is NULL and the error is a `TypesArgumentError` saying which argument failed, in Go's words. Without `validate` nothing is checked, as in Go, and a wrong count panics.

`types_implements`, `types_satisfies` and `types_assertable_to` are the predicates from Go's api_predicates.go. `types_satisfies` is the test for a type argument and `types_implements` the one for an assignment, and they part ways on `comparable`: every interface satisfies it, but an interface only implements it when every type in its type set is strictly comparable.

<!-- example: ../examples/go/types.c#instantiate -->
```c
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
```

That prints:

```
example.com/box.Box[string] same instance: true
struct{v string}
argument 0 []int does not satisfy comparable no type: true
any implements comparable: false satisfies it: true
```

Errors that come with a position are `TypesError`, Go's `types.Error`, whose text is the position and the message. `types_error_as_error` turns one into an `Error`.

## Assignability and conversion

`types_assignable_to` and `types_convertible_to` are Go's `AssignableTo` and `ConvertibleTo`. They ask whether a value of type `V` can be assigned or converted to `T`, using the rules from the spec. The value is never a constant, so nothing is range checked: `untyped int` is assignable to `float32`, and `untyped nil` to any type that has a nil. As in Go, the answer means nothing when either type is invalid or a generic type that has not been instantiated.

<!-- example: ../examples/go/types.c#assign -->
```c
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
```

That prints:

```
MyInt to int: assign false convert true
[]byte to string: assign false convert true
chan int to <-chan int: true and back: false
nil to *int: true to int: false
```

## Type checking

`types_config_check` is Go's `Config.Check`. It takes the parsed files of one package and gives back the package, with every name declared in it given a type. Pass a `TypesInfo` to learn more: `types_info_all` makes one with every map in place, and the checker fills in the type of each expression, what each identifier declares or refers to, and the order the package variables are set in. The package, the info and everything in them come from the allocator you pass, so an arena is the easy way to free them.

<!-- example: ../examples/go/types.c#check -->
```c
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
```

That prints:

```
type S string
var a int
var b S
var c string
func fib(x int) int
init c = "hello"
init b = S(c)
init a = len(b)
b + "!" has type S
```

`a` depends on `b` and `b` on `c`, so `c` is set first, the same order Go uses. `types_eval` is Go's `Eval`: it checks an expression as if it were written in the package, here at package level since the position is `TOKEN_NO_POS`.

With no error function the checker stops at the first error and gives it back through `err`. Set `conf.error` and it calls the function with each error it finds and keeps going, and `err` still holds the first one:

<!-- example: ../examples/go/types.c#errors -->
```c
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
```

That prints:

```
p.go:5:17: cannot use 2 (untyped int constant) as string value in variable declaration
p.go:4:2: declared and not used: x
p.go:5:6: declared and not used: s
first: p.go:5:17: cannot use 2 (untyped int constant) as string value in variable declaration
```

The messages are the same as Go's. Imports go through `conf.importer`, and with no importer every import is an error, `unsafe` included. An importer that gives back `types_unsafe()` for `"unsafe"` is enough for a package that imports nothing else. For anything more, use the one in go/importer.

## Importing packages

`burrow/go/importer.h` is Go's `go/importer`. `importer_for_compiler` with `"source"` gives an importer that finds each imported package with go/build, parses its files and type checks them, importing whatever they import in turn. This program makes a GOPATH with one package in it and checks a file that uses it:

<!-- example: ../examples/go/importer.c#source -->
```c
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
```

That prints:

```
imports example/shapes
var p example/shapes.Point
var n int
```

The importer keeps every package it checks, so `example/shapes` is checked once however many files import it. For the standard library, set `GOROOT` and the packages are found under `$GOROOT/src`, as `build_default` explains. The first import of something like `fmt` takes a while, since everything `fmt` imports is checked too. A package that uses cgo is run through `go tool cgo` first, so that needs the `go` command, as it does in Go.

Go also has the `"gc"` and `"gccgo"` importers, which read the export data the compilers write instead of the source. They are not here yet, and `importer_for_compiler` gives back the zero `TypesImporter` for them, which is Go's nil.
