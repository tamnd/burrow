# Go source

`burrow/go/token.h` is Go's `go/token`, the bottom layer of Go's own tools for reading Go source. It has the tokens of the language and the positions that tie them back to a file, a line and a column. `burrow/go/scanner.h` is Go's `go/scanner`, which turns source into those tokens. `burrow/go/version.h`, Go's `go/version`, compares Go versions, and `burrow/go/constant.h`, Go's `go/constant`, does exact arithmetic on Go constants. `go/ast` and `go/parser` sit on top of the two and will land in this guide when they are ported.

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
Str words[] = {BURROW_S("func"), BURROW_S("Func"), BURROW_S("x1"),
               BURROW_S("1x")};
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
