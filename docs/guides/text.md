# Text

`burrow/text/tabwriter.h` is Go's `text/tabwriter`, which lines text up in columns, and `burrow/text/scanner.h` is Go's `text/scanner`, which splits text into Go style tokens. `burrow/text/template/parse.h` is Go's `text/template/parse`, the parser behind templates. The template engine itself will land in this guide when it is ported.

## Columns

A tabwriter sits in front of another `IoWriter`. You write cells separated by tabs and lines ended by newlines, and it pads each cell so that every column is as wide as its widest cell:

<!-- example: ../examples/text/tabwriter.c#table -->
```c
TabwriterWriter *w = tabwriter_new_writer(a, out, 0, 8, 2, ' ', 0);
IoWriter tw = tabwriter_writer_as_io_writer(w);
fmt_fprintln_v(tw, "NAME\tSIZE\tKIND");
fmt_fprintf_v(tw, "%s\t%d\t%s\n", "burrow.h", 2048, "header");
fmt_fprintf_v(tw, "%s\t%d\t%s\n", "libburrow.a", 1843200, "archive");
fmt_fprintf_v(tw, "%s\t%d\t%s\n", "README.md", 512, "text");
Error err = tabwriter_writer_flush(w);
tabwriter_writer_free(w);
```

That prints:

```
NAME         SIZE     KIND
burrow.h     2048     header
libburrow.a  1843200  archive
README.md    512      text
```

The numbers after `out` are Go's `minwidth`, `tabwidth` and `padding`, then the pad character and the flags. `minwidth` is the narrowest a column gets, `padding` is added to every cell, and `tabwidth` only matters when the padding is made of tabs or `TABWRITER_TAB_INDENT` is on. `tabwriter_writer_as_io_writer` gives you the writer as an `IoWriter`, which is what `fmt_fprintf` and everything else in the library that writes bytes takes.

Nothing reaches `out` until the writer knows how wide the columns are, which in general is not until the end, so `tabwriter_writer_flush` is not optional. It returns the first error `out` gave, if there was one. A tabwriter from `tabwriter_new_writer` needs `tabwriter_writer_free` as well, unless it came from an arena. You can also zero a `TabwriterWriter` of your own and call `tabwriter_writer_init` on it, which is Go's `new(tabwriter.Writer).Init(...)`. Init can be called again to reuse the writer with different settings, and it keeps the memory the writer has already grown.

## Flags

The flags are Go's with a `TABWRITER_` prefix. `TABWRITER_ALIGN_RIGHT` puts the padding before the text, and `TABWRITER_DEBUG` draws a bar between columns, which is the quickest way to see where the writer thinks your columns are:

<!-- example: ../examples/text/tabwriter.c#right -->
```c
TabwriterWriter *w = tabwriter_new_writer(a, out, 5, 0, 1, ' ',
                                          TABWRITER_ALIGN_RIGHT | TABWRITER_DEBUG);
IoWriter tw = tabwriter_writer_as_io_writer(w);
fmt_fprintln_v(tw, "a\tb\tc\t");
fmt_fprintln_v(tw, "123\t12345\t1234567\t");
tabwriter_writer_flush(w);
tabwriter_writer_free(w);
```

```
    a|     b|       c|
  123| 12345| 1234567|
```

`TABWRITER_FILTER_HTML` treats a tag as zero wide and an entity such as `&amp;` as one character. `TABWRITER_DISCARD_EMPTY_COLUMNS` drops a column in which every cell is empty and ended by a vertical tab. `TABWRITER_TAB_INDENT` pads leading empty cells with tabs whatever the pad character is. Text between two `TABWRITER_ESCAPE` bytes (0xff) is passed through as it is, tabs and newlines included, and `TABWRITER_STRIP_ESCAPE` takes the escape bytes out.

When the pad character is `'\t'` the text is always left aligned, as in Go, since where a tab stops is up to whatever displays it.

## Which cells make a column

This is the part that surprises people. A column is not everything in the same position on every line. It is a run of adjacent lines that all have a cell in that position, and a cell only counts if a tab ends it. The text after the last tab on a line is not in any column:

<!-- example: ../examples/text/tabwriter.c#elastic -->
```c
TabwriterWriter *w = tabwriter_new_writer(a, out, 0, 0, 1, '.', TABWRITER_DEBUG);
IoWriter tw = tabwriter_writer_as_io_writer(w);
fmt_fprintln_v(tw, "a\tb\tc");
fmt_fprintln_v(tw, "aa\tbb\tcc");
fmt_fprintln_v(tw, "aaa\t"); /* no cell in column two, so the b column ends */
fmt_fprintln_v(tw, "aaaa\tdddd\teeee");
tabwriter_writer_flush(w);
tabwriter_writer_free(w);
```

```
a....|b..|c
aa...|bb.|cc
aaa..|
aaaa.|dddd.|eeee
```

The first column runs down all four lines. The second stops at `aaa`, which has nothing after its tab, so `dddd` starts a new column that is sized on its own. In the first example the last column needs no padding, so its lines have no trailing tab. Give every line a trailing tab when the last column should be padded too.

## Errors and panics

A write that fails comes back as an error, from `tabwriter_writer_write` or from `tabwriter_writer_flush`, and the count from a write says how much of the input was taken in before the failure. A failed flush empties the writer, so it can be used again. Running out of memory is an error in the same way, `burrow_err_out_of_memory`.

A panic inside `out` is passed on as Go passes it on, as a new panic whose text is `tabwriter: panic during Flush (...)` or `tabwriter: panic during Write (...)` with the original inside the brackets. Negative widths or padding panic in `tabwriter_new_writer` and `tabwriter_writer_init`, as they do in Go.

## Tokens

A `TextScanner` reads from an `IoReader` and splits what it reads into tokens the way Go's lexer would: identifiers, numbers, character literals, strings, raw strings and comments. It is not a Go parser, but it is a quick way to read a small language or a config format that uses Go's rules. Start from a zeroed scanner, point it at a reader with `text_scanner_init` and call `text_scanner_scan` until it says `TEXT_SCANNER_EOF`:

<!-- example: ../examples/text/scanner.c#tokens -->
```c
StringsReader r;
strings_reader_reset(&r, BURROW_S("\n// This is scanned code.\nif a > 10 {\n"
                                  "\tsomeParsable = text\n}"));
TextScanner s = {0};
text_scanner_init(&s, strings_reader_as_io_reader(&r));
s.position.filename = BURROW_S("example");
Rune tok;
while ((tok = text_scanner_scan(&s)) != TEXT_SCANNER_EOF)
    fmt_printf_v("%s: %s\n", text_scanner_position_string(s.position, a),
                 text_scanner_token_text(&s));
text_scanner_free(&s);
```

That prints:

```
example:3:1: if
example:3:4: a
example:3:6: >
example:3:8: 10
example:3:11: {
example:4:2: someParsable
example:4:15: =
example:4:17: text
example:5:1: }
```

The `position` field says where the last token started. Its filename is yours to set, and `text_scanner_init` leaves it alone. The text from `text_scanner_token_text` points into the scanner and only lasts until the next scan, so copy it with `strings_clone` if you want to keep it. A scanner takes memory only for a token that does not fit in its one kilobyte buffer, from the allocator in its `a` field, which is the heap when you leave it zeroed. `text_scanner_free` gives that memory back.

## What kind of token

`text_scanner_scan` returns a negative `TEXT_SCANNER_` constant for a token and the character itself for anything else, so `=` comes back as `'='`. Comments are skipped by default. `text_scanner_token_string` names a token the way Go's `scanner.TokenString` does:

<!-- example: ../examples/text/scanner.c#kinds -->
```c
StringsReader r;
strings_reader_reset(&r, BURROW_S("x := 3.5e2 + 'a' // half\nname = `raw` \"q\""));
TextScanner s = {0};
text_scanner_init(&s, strings_reader_as_io_reader(&r));
Rune tok;
while ((tok = text_scanner_scan(&s)) != TEXT_SCANNER_EOF)
    fmt_printf_v("%-8s %s\n", text_scanner_token_string(a, tok),
                 text_scanner_token_text(&s));
text_scanner_free(&s);
```

That prints:

```
Ident    x
":"      :
"="      =
Float    3.5e2
"+"      +
Char     'a'
Ident    name
"="      =
RawString `raw`
String   "q"
```

The `mode` field picks which tokens the scanner knows, from the `TEXT_SCANNER_SCAN_` bits, and starts as `TEXT_SCANNER_GO_TOKENS`. Take `TEXT_SCANNER_SKIP_COMMENTS` out of it to get comments back as `TEXT_SCANNER_COMMENT` tokens. The `whitespace` field is a bit set of the characters to skip, so clearing the bit for `'\n'` makes newlines come back as tokens, which is how you read line based input. The `is_ident_rune` field takes a function that says what an identifier is made of, when Go's letters and digits are not what you want.

## Errors

A bad token does not stop the scan. The scanner calls the function in its `error` field with a message, counts the error in `error_count` and carries on. The message is only good for the length of the call. With no error function the scanner prints the position and message to stderr, as Go does.

<!-- example: ../examples/text/scanner.c#report -->
```c
static void report(void *env, TextScanner *s, Str msg) {
    Alloc *a = env;
    fmt_printf_v("error at %s: %s\n",
                 text_scanner_position_string(text_scanner_pos(s), a), msg);
}
```

<!-- example: ../examples/text/scanner.c#errors -->
```c
StringsReader r;
strings_reader_reset(&r, BURROW_S("ok 0x \"open"));
TextScanner s = {0};
text_scanner_init(&s, strings_reader_as_io_reader(&r));
s.position.filename = BURROW_S("input");
s.error = BURROW_FN(TextScannerErrorFunc, report, a);
while (text_scanner_scan(&s) != TEXT_SCANNER_EOF) {
}
fmt_println_v("errors:", s.error_count);
text_scanner_free(&s);
```

That prints:

```
error at input:1:6: hexadecimal literal has no digits
error at input:1:12: literal not terminated
errors: 2
```

## Template parse trees

`burrow/text/template/parse.h` is Go's `text/template/parse`, the parser that `text/template` and `html/template` are built on. Most programs will want those packages, but the parser is handy on its own when you need to look inside a template: to list the fields it reads, to catch syntax errors before the template is used, or to write a tool that rewrites templates.

`parse_parse` takes the name and text of a template and returns a `Map` from names to `ParseTree` pointers. The template itself gets a tree, and so does each `{{define}}` inside it. Empty delimiters mean `{{` and `}}`. Writing a tree's root list back out gives you template text again, in a tidied form:

<!-- example: ../examples/text/template_parse.c#trees -->
```c
Str text = BURROW_S("{{define \"item\"}}<li>{{.}}</li>{{end}}"
                    "<ul>{{range .Items}}{{template \"item\" .}}{{end}}</ul>");
Error err = BURROW_NO_ERROR;
Map *trees = parse_parse(a, BURROW_S("page"), text, BURROW_STR_EMPTY,
                         BURROW_STR_EMPTY, (Slice){0}, &err);
if (BURROW_FAILED(err)) {
    fmt_println_v(err);
    return;
}
Str names[] = {BURROW_S("page"), BURROW_S("item")};
for (int i = 0; i < 2; i++) {
    ParseTree **t = map_get(trees, &names[i]);
    fmt_printf_v("%s: %s\n", names[i], parse_list_node_string((*t)->root, a));
}
```

That prints:

```
page: <ul>{{range .Items}}{{template "item" .}}{{end}}</ul>
item: <li>{{.}}</li>
```

## Walking a tree

A `ParseNode` points at the `ParseNodeType` every node starts with, so `*n` says what the node is, and the node can then be cast to its own struct, such as `ParseActionNode` or `ParseIfNode`. To parse with options, make the tree with `parse_new`, set its `mode` and fill it in with `parse_tree_parse`. `PARSE_SKIP_FUNC_CHECK` lets the template call functions the parser has not been told about, and `PARSE_PARSE_COMMENTS` keeps comments in the tree as `PARSE_NODE_COMMENT` nodes:

<!-- example: ../examples/text/template_parse.c#walk -->
```c
Error err = BURROW_NO_ERROR;
ParseTree *t = parse_new(a, BURROW_S("greeting"), (Slice){0});
t->mode = PARSE_SKIP_FUNC_CHECK; // len is not defined here
t = parse_tree_parse(
    t, BURROW_S("Hello, {{.Name}}! You have {{len .Mail}} letters."),
    BURROW_STR_EMPTY, BURROW_STR_EMPTY,
    map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0), (Slice){0}, &err);
if (t == NULL) {
    fmt_println_v(err);
    return;
}
for (Int i = 0; i < t->root->nodes.len; i++) {
    ParseNode n = BURROW_AT(ParseNode, t->root->nodes, i);
    switch (*n) {
    case PARSE_NODE_TEXT:
        fmt_printf_v("text   %q\n", parse_node_string(n, a));
        break;
    case PARSE_NODE_ACTION: {
        ParseActionNode *act = (ParseActionNode *)n;
        ParseCommandNode *cmd = BURROW_AT(ParseCommandNode *, act->pipe->cmds, 0);
        fmt_printf_v("action %s, %d argument(s)\n", parse_node_string(n, a),
                     cmd->args.len);
        break;
    }
    default:
        break;
    }
}
```

That prints:

```
text   "Hello, "
action {{.Name}}, 1 argument(s)
text   "! You have "
action {{len .Mail}}, 2 argument(s)
text   " letters."
```

## Template errors

A syntax error stops the parse. It comes back in `*err` with the template name and line in front of the message, the same text Go gives. `parse_parse` still returns the trees it finished before the error, and `parse_tree_parse` returns NULL:

<!-- example: ../examples/text/template_parse.c#errors -->
```c
Str bad[] = {BURROW_S("{{if .Ready}}go"), BURROW_S("{{.Name | shout}}"),
             BURROW_S("line one\n{{.X 1 2 3 ) }}")};
for (int i = 0; i < 3; i++) {
    Error err = BURROW_NO_ERROR;
    parse_parse(a, BURROW_S("bad"), bad[i], BURROW_STR_EMPTY, BURROW_STR_EMPTY,
                (Slice){0}, &err);
    fmt_println_v(err);
}
```

That prints:

```
template: bad:1: unexpected EOF
template: bad:1: function "shout" not defined
template: bad:2: unexpected right paren
```

The parser checks that every function a template calls exists, but it only looks at names. The `funcs` argument is a slice of `Map` pointers, each from `Str` to `Any`, and a name counts as defined when one of the maps holds a value for it that is not nil. What the value is does not matter to the parser:

<!-- example: ../examples/text/template_parse.c#funcs -->
```c
Map *fm = map_make(a, TYPE_STRING, TYPE_ANY, 0);
Str name = BURROW_S("shout");
Any f = BURROW_ANY_VAL(TYPE_INT, Int, 1); // only the name matters to the parser
map_set(fm, &name, &f);
Error err = BURROW_NO_ERROR;
Map *trees =
    parse_parse(a, BURROW_S("ok"), BURROW_S("{{.Name | shout}}"), BURROW_STR_EMPTY,
                BURROW_STR_EMPTY, (Slice){&fm, 1, 1, TYPE_UNSAFE_POINTER}, &err);
Str ok = BURROW_S("ok");
ParseTree **t = map_get(trees, &ok);
fmt_println_v(err, parse_list_node_string((*t)->root, a));
```

That prints:

```
<nil> {{.Name | shout}}
```
