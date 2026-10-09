# Go source

`burrow/go/token.h` is Go's `go/token`, the bottom layer of Go's own tools for reading Go source. It has the tokens of the language and the positions that tie them back to a file, a line and a column. `go/scanner`, `go/ast` and `go/parser` sit on top of it and will land in this guide when they are ported.

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
