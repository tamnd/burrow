# Text

`burrow/text/tabwriter.h` is Go's `text/tabwriter`, which lines text up in columns, and `burrow/text/scanner.h` is Go's `text/scanner`, which splits text into Go style tokens. `burrow/text/template/parse.h` is Go's `text/template/parse`, the parser behind templates. `burrow/text/template.h` is Go's `text/template`, the template engine built on it.

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

## Templates

`burrow/text/template.h` is Go's `text/template`. A template is text with actions between `{{` and `}}`, and the actions read the data the template runs with: `.Name` is a field, `{{if}}` and `{{with}}` test a value, `{{range}}` loops over one, and a `-` next to a delimiter trims the space on that side. The language and its output are Go's. The template reads the data through reflect, so the data's type needs a descriptor. Here `BURROW_STRUCT` gives a struct one:

<!-- example: ../examples/text/template.c#types -->
```c
#define RECIPIENT_FIELDS(F, T)                                                         \
    F(T, Str, Name, "")                                                                \
    F(T, Str, Gift, "")                                                                \
    F(T, bool, Attended, "")
BURROW_STRUCT(Recipient, RECIPIENT_FIELDS);
```

`text_template_new` makes a template in an allocator, `text_template_parse` parses text into it, and `text_template_must` panics if the parse failed, the way `template.Must` does. `text_template_execute` runs the template, writes to an `IoWriter`, and takes the data as an `Any`:

<!-- example: ../examples/text/template.c#letter -->
```c
// Define a template.
Str text = BURROW_S("\n"
                    "Dear {{.Name}},\n"
                    "{{if .Attended}}\n"
                    "It was a pleasure to see you at the wedding.\n"
                    "{{- else}}\n"
                    "It is a shame you couldn't make it to the wedding.\n"
                    "{{- end}}\n"
                    "{{with .Gift -}}\n"
                    "Thank you for the lovely {{.}}.\n"
                    "{{end}}\n"
                    "Best wishes,\n"
                    "Josie\n");

// Prepare some data to insert into the template.
Recipient recipients[] = {
    {BURROW_S("Aunt Mildred"), BURROW_S("bone china tea set"), true},
    {BURROW_S("Uncle John"), BURROW_S("moleskin pants"), false},
    {BURROW_S("Cousin Rodney"), BURROW_STR_EMPTY, false},
};

// Create a new template and parse the letter into it.
Error err = BURROW_NO_ERROR;
TextTemplate *t =
    text_template_parse(text_template_new(a, BURROW_S("letter")), text, &err);
t = text_template_must(t, err);

// Execute the template for each recipient.
for (int i = 0; i < 3; i++) {
    err = text_template_execute(t, out,
                                BURROW_ANY(TYPE_OF(Recipient), &recipients[i]));
    if (BURROW_FAILED(err))
        fmt_println_v("executing template:", err);
}
```

That prints the three letters:

```

Dear Aunt Mildred,

It was a pleasure to see you at the wedding.
Thank you for the lovely bone china tea set.

Best wishes,
Josie

Dear Uncle John,

It is a shame you couldn't make it to the wedding.
Thank you for the lovely moleskin pants.

Best wishes,
Josie

Dear Cousin Rodney,

It is a shame you couldn't make it to the wedding.

Best wishes,
Josie
```

## Functions

Besides the builtins (`printf`, `len`, `index`, `eq` and the rest), a template can call functions from a `TextTemplateFuncMap`, a `Map` from names to `Any`. Each `Any` holds a function value whose type was declared with `BURROW_FUNC` and `BURROW_FUNC_TYPE_DEFINE`, so that the template knows its parameters and results. The function's environment pointer is a good place for the allocator its result is made in:

<!-- example: ../examples/text/template.c#functypes -->
```c
// func(string) string and func([]string, string) string. The environment of
// each function value is the allocator its result is made in.
#define CASE_SIG(IN, OUT) IN(0, Str) OUT(Str)
BURROW_FUNC(CaseFunc, Str, Str s);
BURROW_FUNC_TYPE_DEFINE(CaseFunc, CASE_SIG);

BURROW_SLICE_TYPE(Names, Str);
#define JOIN_SIG(IN, OUT) IN(0, Names) IN(1, Str) OUT(Str)
BURROW_FUNC(JoinFunc, Str, Slice elems, Str sep);
BURROW_FUNC_TYPE_DEFINE(JoinFunc, JOIN_SIG);
```

`text_template_funcs` adds the map, and it has to be called before the text that uses the functions is parsed:

<!-- example: ../examples/text/template.c#func -->
```c
// First we create a FuncMap with which to register the function.
CaseFunc title_fn = BURROW_FN(CaseFunc, title, a);
Map *func_map = map_make(a, TYPE_STRING, TYPE_ANY, 1);
// The name "title" is what the function will be called in the template text.
set_func(func_map, "title", BURROW_ANY(TYPE_OF(CaseFunc), &title_fn));

// A simple template definition to test our function. We print the input
// text several ways: the original, title-cased, title-cased and then
// printed with %q, and printed with %q and then title-cased.
Str template_text = BURROW_S("\n"
                             "Input: {{printf \"%q\" .}}\n"
                             "Output 0: {{title .}}\n"
                             "Output 1: {{title . | printf \"%q\"}}\n"
                             "Output 2: {{printf \"%q\" . | title}}\n");

// Create a template, add the function map, and parse the text.
Error err = BURROW_NO_ERROR;
TextTemplate *tmpl = text_template_parse(
    text_template_funcs(text_template_new(a, BURROW_S("titleTest")), func_map),
    template_text, &err);
if (BURROW_FAILED(err)) {
    fmt_printf_v("parsing: %s\n", err);
    return;
}

// Run the template to verify the output.
Str data = BURROW_S("the go programming language");
err = text_template_execute(tmpl, out, BURROW_ANY(TYPE_STRING, &data));
if (BURROW_FAILED(err))
    fmt_printf_v("execution: %s\n", err);
```

That prints:

```

Input: "the go programming language"
Output 0: The Go Programming Language
Output 1: "The Go Programming Language"
Output 2: "The Go Programming Language"
```

Calling `text_template_funcs` again after the parse replaces functions by name, so a template, or a clone of one, can be pointed at different code:

<!-- example: ../examples/text/template.c#funcs -->
```c
// Define a simple template to test the functions.
Str tmpl = BURROW_S("{{ . | lower | repeat }}");

// Define the template FuncMap with two functions.
CaseFunc lower_fn = BURROW_FN(CaseFunc, lower, a);
CaseFunc repeat2_fn = BURROW_FN(CaseFunc, repeat2, a);
Map *func_map = map_make(a, TYPE_STRING, TYPE_ANY, 2);
set_func(func_map, "lower", BURROW_ANY(TYPE_OF(CaseFunc), &lower_fn));
set_func(func_map, "repeat", BURROW_ANY(TYPE_OF(CaseFunc), &repeat2_fn));

// Define a new template, add the FuncMap with text_template_funcs and then
// parse the template.
Error err = BURROW_NO_ERROR;
TextTemplate *parsed = text_template_parse(
    text_template_funcs(text_template_new(a, BURROW_S("t")), func_map), tmpl, &err);
if (BURROW_FAILED(err)) {
    fmt_println_v(err);
    return;
}
Str abc = BURROW_S("ABC\n");
err = text_template_execute(parsed, out, BURROW_ANY(TYPE_STRING, &abc));
if (BURROW_FAILED(err)) {
    fmt_println_v(err);
    return;
}

// text_template_funcs must be called before a template is parsed to add
// functions to it, and it can also be called after to overwrite them.
// Here the function named "repeat" is overwritten.
CaseFunc repeat3_fn = BURROW_FN(CaseFunc, repeat3, a);
Map *overwrite = map_make(a, TYPE_STRING, TYPE_ANY, 1);
set_func(overwrite, "repeat", BURROW_ANY(TYPE_OF(CaseFunc), &repeat3_fn));
text_template_funcs(parsed, overwrite);
Str def = BURROW_S("DEF\n");
err = text_template_execute(parsed, out, BURROW_ANY(TYPE_STRING, &def));
if (BURROW_FAILED(err))
    fmt_println_v(err);
```

That prints:

```
abc
abc
def
def
def
```

The comparison builtins work across the integer and float kinds the way Go's do, so a `float` field compares with the constant `4.0`:

<!-- example: ../examples/text/template.c#if -->
```c
Error err = BURROW_NO_ERROR;
TextTemplate *tpl = text_template_parse(
    text_template_new(a, BURROW_S("book")),
    BURROW_S("{{ if (gt .Stars 4.0) }}\"{{.Name }}\" is a great book.{{ else }}"
             "\"{{.Name}}\" is not a great book.{{ end }}"),
    &err);
if (BURROW_FAILED(err)) {
    fmt_printf_v("failed to parse template: %s\n", err);
    return;
}

Book book = {4.9F, BURROW_S("Good Night, Gopher")};
BookPtr b = &book;
err = text_template_execute(tpl, out, BURROW_ANY(TYPE_OF(BookPtr), &b));
if (BURROW_FAILED(err))
    fmt_printf_v("failed to execute template: %s\n", err);
fmt_fprint_v(out, BURROW_S("\n"));
```

That prints:

```
"Good Night, Gopher" is a great book.
```

## Template sets

A template can define others with `{{define}}`, and the templates parsed together share one name space, so `{{template "name" .}}` can call any of them. `{{block}}` defines a template and calls it in one go, which is how a base template leaves a part for others to fill in. `text_template_clone` copies the whole set, and parsing a new definition into the copy leaves the original alone:

<!-- example: ../examples/text/template.c#block -->
```c
Str master = BURROW_S("Names:{{block \"list\" .}}{{\"\\n\"}}{{range .}}"
                      "{{println \"-\" .}}{{end}}{{end}}");
Str overlay = BURROW_S("{{define \"list\"}} {{join . \", \"}}{{end}} ");
JoinFunc join_fn = BURROW_FN(JoinFunc, join, a);
Map *funcs = map_make(a, TYPE_STRING, TYPE_ANY, 1);
set_func(funcs, "join", BURROW_ANY(TYPE_OF(JoinFunc), &join_fn));
Str guardians[] = {BURROW_S("Gamora"), BURROW_S("Groot"), BURROW_S("Nebula"),
                   BURROW_S("Rocket"), BURROW_S("Star-Lord")};
Slice names = slice_from(guardians, 5, 5, TYPE_STRING);
Any data = BURROW_ANY(TYPE_OF(Names), &names);

Error err = BURROW_NO_ERROR;
TextTemplate *master_tmpl = text_template_parse(
    text_template_funcs(text_template_new(a, BURROW_S("master")), funcs), master,
    &err);
if (BURROW_FAILED(err)) {
    fmt_println_v(err);
    return;
}
TextTemplate *clone = text_template_clone(master_tmpl, a, &err);
TextTemplate *overlay_tmpl =
    text_template_parse(text_template_must(clone, err), overlay, &err);
if (BURROW_FAILED(err)) {
    fmt_println_v(err);
    return;
}
err = text_template_execute(master_tmpl, out, data);
if (BURROW_FAILED(err)) {
    fmt_println_v(err);
    return;
}
// The overlay ends in a space, which is trimmed here so that the line does
// not end in one.
StringsBuilder b = STRINGS_BUILDER(a);
err = text_template_execute(overlay_tmpl, strings_builder_as_io_writer(&b), data);
if (BURROW_FAILED(err)) {
    fmt_println_v(err);
    return;
}
fmt_fprint_v(out, strings_trim_suffix(strings_builder_string(&b), BURROW_S(" ")));
```

That prints:

```
Names:
- Gamora
- Groot
- Nebula
- Rocket
- Star-Lord
Names: Gamora, Groot, Nebula, Rocket, Star-Lord
```

Templates usually live in files. `text_template_from_glob` parses every file a pattern matches, each one into a template named after the file, and returns the first. `text_template_from_files` takes a list of paths and `text_template_from_fs` reads from an `Fs`. The `parse_` versions of the three add the files to a template that already exists:

<!-- example: ../examples/text/template.c#glob -->
```c
// Here we create a temporary directory and fill it with our sample
// template definition files. Usually the template files would already
// exist in some location known to the program.
static const TemplateFile files[] = {
    // T0.tmpl is a plain template file that just invokes T1.
    {"T0.tmpl", "T0 invokes T1: ({{template \"T1\"}})"},
    // T1.tmpl defines a template, T1 that invokes T2.
    {"T1.tmpl", "{{define \"T1\"}}T1 invokes T2: ({{template \"T2\"}}){{end}}"},
    // T2.tmpl defines a template T2.
    {"T2.tmpl", "{{define \"T2\"}}This is T2{{end}}"},
};
Str dir = create_test_dir(a, files, 3);

// pattern is the glob pattern used to find all the template files.
Str pattern = tmpl_pattern(a, dir);

// T0.tmpl is the first name matched, so it becomes the starting template,
// the value text_template_from_glob returns.
Error err = BURROW_NO_ERROR;
TextTemplate *tmpl = text_template_from_glob(a, pattern, &err);
tmpl = text_template_must(tmpl, err);

err = text_template_execute(tmpl, out, (Any){NULL, NULL});
if (BURROW_FAILED(err))
    fmt_printf_v("template execution: %s\n", err);
fmt_fprint_v(out, BURROW_S("\n"));
(void)os_remove_all(dir);
```

That prints:

```
T0 invokes T1: (T1 invokes T2: (This is T2))
```

Driver templates can be added to a loaded set with more calls to `text_template_parse`, and `text_template_execute_template` runs a template of the set by name:

<!-- example: ../examples/text/template.c#helpers -->
```c
static const TemplateFile files[] = {
    // T1.tmpl defines a template, T1 that invokes T2.
    {"T1.tmpl", "{{define \"T1\"}}T1 invokes T2: ({{template \"T2\"}}){{end}}"},
    // T2.tmpl defines a template T2.
    {"T2.tmpl", "{{define \"T2\"}}This is T2{{end}}"},
};
Str dir = create_test_dir(a, files, 2);
Str pattern = tmpl_pattern(a, dir);

// Load the helpers.
Error err = BURROW_NO_ERROR;
TextTemplate *templates = text_template_from_glob(a, pattern, &err);
templates = text_template_must(templates, err);
// Add one driver template to the bunch, with an explicit template
// definition.
text_template_parse(
    templates,
    BURROW_S("{{define `driver1`}}Driver 1 calls T1: ({{template `T1`}})\n{{end}}"),
    &err);
if (BURROW_FAILED(err))
    fmt_println_v("parsing driver1:", err);
// Add another driver template.
text_template_parse(
    templates,
    BURROW_S("{{define `driver2`}}Driver 2 calls T2: ({{template `T2`}})\n{{end}}"),
    &err);
if (BURROW_FAILED(err))
    fmt_println_v("parsing driver2:", err);
// We load all the templates before execution. This package does not
// require that, but html/template's escaping does, so it's a good habit.
err = text_template_execute_template(templates, out, BURROW_S("driver1"),
                                     (Any){NULL, NULL});
if (BURROW_FAILED(err))
    fmt_printf_v("driver1 execution: %s\n", err);
err = text_template_execute_template(templates, out, BURROW_S("driver2"),
                                     (Any){NULL, NULL});
if (BURROW_FAILED(err))
    fmt_printf_v("driver2 execution: %s\n", err);
(void)os_remove_all(dir);
```

That prints:

```
Driver 1 calls T1: (T1 invokes T2: (This is T2))
Driver 2 calls T2: (This is T2)
```

One set of drivers can run with different helpers by cloning it once for each:

<!-- example: ../examples/text/template.c#share -->
```c
static const TemplateFile files[] = {
    // T0.tmpl is a plain template file that just invokes T1.
    {"T0.tmpl", "T0 ({{.}} version) invokes T1: ({{template `T1`}})\n"},
    // T1.tmpl defines a template, T1 that invokes T2. Note T2 is not defined.
    {"T1.tmpl", "{{define \"T1\"}}T1 invokes T2: ({{template \"T2\"}}){{end}}"},
};
Str dir = create_test_dir(a, files, 2);
Str pattern = tmpl_pattern(a, dir);

// Load the drivers.
Error err = BURROW_NO_ERROR;
TextTemplate *drivers = text_template_from_glob(a, pattern, &err);
drivers = text_template_must(drivers, err);

// We must define an implementation of the T2 template. First we clone the
// drivers, then add a definition of T2 to the template name space.

// 1. Clone the helper set to create a new name space from which to run them.
TextTemplate *first = text_template_clone(drivers, a, &err);
if (BURROW_FAILED(err))
    fmt_println_v("cloning helpers:", err);
// 2. Define T2, version A, and parse it.
text_template_parse(first, BURROW_S("{{define `T2`}}T2, version A{{end}}"), &err);
if (BURROW_FAILED(err))
    fmt_println_v("parsing T2:", err);

// Now repeat the whole thing, using a different version of T2.
// 1. Clone the drivers.
TextTemplate *second = text_template_clone(drivers, a, &err);
if (BURROW_FAILED(err))
    fmt_println_v("cloning drivers:", err);
// 2. Define T2, version B, and parse it.
text_template_parse(second, BURROW_S("{{define `T2`}}T2, version B{{end}}"), &err);
if (BURROW_FAILED(err))
    fmt_println_v("parsing T2:", err);

// Execute the templates in the reverse order to verify the first is
// unaffected by the second.
Str s2 = BURROW_S("second");
err = text_template_execute_template(second, out, BURROW_S("T0.tmpl"),
                                     BURROW_ANY(TYPE_STRING, &s2));
if (BURROW_FAILED(err))
    fmt_printf_v("second execution: %s\n", err);
Str s1 = BURROW_S("first");
err = text_template_execute_template(first, out, BURROW_S("T0.tmpl"),
                                     BURROW_ANY(TYPE_STRING, &s1));
if (BURROW_FAILED(err))
    fmt_printf_v("first: execution: %s\n", err);
(void)os_remove_all(dir);
```

That prints:

```
T0 (second version) invokes T1: (T1 invokes T2: (T2, version B))
T0 (first version) invokes T1: (T1 invokes T2: (T2, version A))
```

## Execution errors

A failed parse comes back in `*err` with the template name and line, and so does a failure while the template runs, as a `TextTemplateExecError` that says which template and action it was in, such as `template: top:7:20: executing "three" at <index "hi" $>: error calling index: index out of range: 5`. A function that returns an error, or panics, stops the template the same way. The output written before the error stays written, so render into a `StringsBuilder` first when half a page is worse than none. A field or key the data does not have is an error too, except a missing map key, which prints `<no value>`. `text_template_option` with `missingkey=zero` or `missingkey=error` changes that.

A template is safe to execute from several threads at once. Parsing, `text_template_funcs` and the other calls that change a template must not overlap with an execution of it.

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
