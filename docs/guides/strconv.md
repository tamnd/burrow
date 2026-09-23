# Converting to and from text

`burrow/strconv.h` is Go's `strconv`: the package that writes values as text and reads them back. It never looks at the C locale, so a program that calls `setlocale` gets the same answers as one that does not, which is the first thing that goes wrong with `printf` and `strtod`.

This page covers quoting, the part that has landed so far. Integers, booleans and floats come next, in that order.

## Quoting

`strconv_quote` gives you the Go string literal for a string, quotes included:

<!-- example: ../examples/strconv/strconv.c#quote -->
```c
Str q = strconv_quote(a, BURROW_S("tab\there, bell\a, ☺"));
```

That prints `"tab\there, bell\a, ☺"`. Printable runes go through as they are, and everything else becomes the escape Go would write: the short ones like `\n` where there is one, `\x` for a single byte, and `\u` or `\U` for any other rune.

There are three flavours and each has a rune form:

<!-- example: ../examples/strconv/strconv.c#variants -->
```c
Str ascii = strconv_quote_to_ascii(a, BURROW_S("naïve ☺"));
Str rune = strconv_quote_rune(a, 0x263a);
Str bad = strconv_quote(a, BURROW_S("\xff\xfe"));
```

The ASCII form escapes everything outside ASCII, which is what you want for a log line that might end up in a terminal with the wrong encoding. The graphic form, `strconv_quote_to_graphic`, lets through the spaces that Unicode calls graphic and Go calls unprintable, such as the no-break space. The rune forms write a single quoted character literal, and a rune that is not a valid code point is quoted as U+FFFD.

Bytes that are not valid UTF-8 come out as `\x` escapes, one per byte, so `bad` above is `"\xff\xfe"`. That is what makes quoting safe to use on anything: the literal always unquotes back to exactly the bytes you started with.

Every quoting function allocates its result from `a`, sized to the byte, so a heap allocated result can be freed with its own length. Each also has an append form that adds the literal to a byte slice the way `append` does:

<!-- example: ../examples/strconv/strconv.c#append -->
```c
Slice line = slice_from_str(a, BURROW_S("key="));
line = strconv_append_quote(a, line, BURROW_S("a \"quoted\" value"));
```

It writes in place when the slice has room and moves to a new array from `a` when it does not, so keep using the slice it returns. A zero `Slice` is fine as the starting point and means Go's nil `[]byte`.

## Unquoting

`strconv_unquote` reads a whole literal back: double quoted, single quoted or backquoted.

<!-- example: ../examples/strconv/strconv.c#unquote -->
```c
Error err;
Str s = strconv_unquote(a, BURROW_S("\"caf\\u00e9\\n\""), &err);
if (BURROW_FAILED(err))
    printf("not a literal: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
```

Anything that is not exactly one well formed literal is `strconv_err_syntax`, including a good literal with something after it. Compare with `errors_is`, the same as for any other sentinel:

<!-- example: ../examples/strconv/strconv.c#syntax -->
```c
(void)strconv_unquote(a, BURROW_S("\"unterminated"), &err);
if (errors_is(err, strconv_err_syntax))
    printf("bad literal\n");
```

When there is something after the literal and you want both, `strconv_quoted_prefix` finds where the literal ends. It gives you the literal itself, quotes and escapes and all, as a view of the input:

<!-- example: ../examples/strconv/strconv.c#prefix -->
```c
Str rest = BURROW_S("\"first\" and then the rest");
Str lit = strconv_quoted_prefix(rest, NULL);
```

One thing about `strconv_unquote` needs care. When the literal has nothing to undo, no escapes and no carriage returns in a raw string, the answer is the inside of the literal, and that is what you get back, pointing into your input with nothing allocated:

<!-- example: ../examples/strconv/strconv.c#borrow -->
```c
Str raw = BURROW_S("`C:\\no\\escapes`");
Str inside = strconv_unquote(a, raw, NULL); /* points into raw */
```

Go does the same, and there it is invisible. Here it means the result can borrow from `s` or own memory from `a`, and you cannot tell which from the call. The header says both, `BURROW_OWNS(ret) BURROW_BORROWS(ret, s)`, which is the same rule as for append: keep the input alive for as long as you use the result, and use an arena so that there is nothing to free by hand. If you need a result that is always your own, `str_clone` it.

For reading a literal one character at a time, which is what a tokenizer does, there is `strconv_unquote_char`. It takes the text inside the quotes and gives back the first character, whether it needs more than one byte of UTF-8, and the rest of the text.

## Printable runes

<!-- example: ../examples/strconv/strconv.c#print -->
```c
bool p = strconv_is_print(0x00a0);   /* no-break space: false */
bool g = strconv_is_graphic(0x00a0); /* true */
```

`strconv_is_print` is Go's definition of printable, the same set as `unicode.IsPrint`: letters, marks, numbers, punctuation, symbols and the ASCII space, and no other space. `strconv_is_graphic` adds the other spaces. Both run from Go's own compact tables, generated from `isprint.go` by `tools/gen-isprint.sh`, so they follow the Unicode version of the Go release burrow tracks. The test suite checks every one of the 1,114,112 code points against Go's answer.

`strconv_can_backquote` says whether a string can go between backquotes unchanged, which means one line, no control characters other than tab and no byte order mark.
