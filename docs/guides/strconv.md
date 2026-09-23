# Converting to and from text

`burrow/strconv.h` is Go's `strconv`: the package that writes values as text and reads them back. It never looks at the C locale, so a program that calls `setlocale` gets the same answers as one that does not, which is the first thing that goes wrong with `printf` and `strtod`.

This page covers integers, booleans, floats, complex numbers and quoting.

## Integers

`strconv_parse_int` reads a signed integer in any base from 2 to 36 and checks that it fits in the number of bits you ask for. Base 0 means the base comes from the prefix, the way Go source writes it, and then underscores between digits are allowed too:

<!-- example: ../examples/strconv/strconv.c#parse -->
```c
Error err;
int64_t n = strconv_parse_int(BURROW_S("-0x_7f"), 0, 8, &err);
```

That is -127, which fits in 8 bits. A bit size of 0 means `Int`, whose width is `STRCONV_INT_SIZE`. `strconv_parse_uint` is the same without the sign, and `strconv_atoi` is `strconv_parse_int(s, 10, 0, err)` with a faster path for the short numbers most input is made of.

A number that is well formed but too big gives `strconv_err_range`, and the value you get back is the closest one that fits rather than zero:

<!-- example: ../examples/strconv/strconv.c#range -->
```c
int64_t big = strconv_parse_int(BURROW_S("300"), 10, 8, &err);
if (errors_is(err, strconv_err_range))
    printf("clamped to %lld: " BURROW_STR_FMT "\n", (long long)big,
           BURROW_STR_ARG(error_text(err)));
```

That prints `clamped to 127: strconv.ParseInt: parsing "300": value out of range`. Bad input gives `strconv_err_syntax` and 0.

Both come wrapped in a `StrconvNumError`, which is Go's `*NumError`: the name of the function, a copy of the input and the reason. `errors_is` sees through it to the reason, as above, and `errors_as` gets you the struct:

<!-- example: ../examples/strconv/strconv.c#numerror -->
```c
(void)strconv_atoi(BURROW_S("12a"), &err);
const StrconvNumError *ne = errors_as(err, TYPE_STRCONV_NUM_ERROR);
if (ne != NULL)
    printf(BURROW_STR_FMT " failed on " BURROW_STR_FMT "\n",
           BURROW_STR_ARG(ne->func), BURROW_STR_ARG(ne->num));
```

The error is built in the calling goroutine's error arena, so the parse functions take no allocator for it, and it is only built at all when you pass somewhere to put it. With `NULL` for `err` a failed parse costs nothing more than a successful one. To keep an error past the end of the goroutine, `error_retain` it.

Going the other way, `strconv_format_int` and `strconv_format_uint` write a number in any base from 2 to 36, `strconv_itoa` is base 10 for an `Int`, and the append forms add the digits to a byte slice:

<!-- example: ../examples/strconv/strconv.c#format -->
```c
Str hex = strconv_format_int(a, -255, 16);
Str dec = strconv_itoa(a, 1234567);
Slice line = strconv_append_uint(a, slice_from_str(a, BURROW_S("id=")), 42, 10);
```

That gives `-ff`, `1234567` and `id=42`. A base outside 2 to 36 is a panic, as it is in Go, because it is a bug in the caller and not something the input did. Results are allocated from `a` at exactly their length.

## Booleans

<!-- example: ../examples/strconv/strconv.c#bool -->
```c
bool on = strconv_parse_bool(BURROW_S("True"), NULL);
Str text = strconv_format_bool(on);
```

`strconv_parse_bool` takes `1`, `t`, `T`, `TRUE`, `true` and `True` and the same six spellings of false, and anything else is `strconv_err_syntax` in a `StrconvNumError`. `strconv_format_bool` returns one of two literals, so it needs no allocator and there is nothing to free. `strconv_append_bool` adds the word to a byte slice.

## Floats

`strconv_parse_float` reads a decimal or hexadecimal floating point number and rounds it correctly to the nearest double, or to the nearest float when the bit size is 32. `strconv_format_float` goes the other way, and with a precision of -1 it writes the fewest digits that read back as exactly the same value:

<!-- example: ../examples/strconv/strconv.c#float -->
```c
Error err;
double f = strconv_parse_float(BURROW_S("0.1"), 64, &err);
Str shortest = strconv_format_float(a, f, 'g', -1, 64);
Str exact = strconv_format_float(a, f, 'f', 20, 64);
Str hex = strconv_format_float(a, f, 'x', -1, 64);
```

That gives `0.1`, `0.10000000000000000555` and `0x1.999999999999ap-04`. The first is the shortest text that parses back to the same double, the second shows the digits the double really holds, and the third is its bits in hexadecimal. The format letters are Go's: `e`, `E`, `f`, `g`, `G`, `b`, `x` and `X`, with the table in the header. Neither direction looks at the locale, so the point is always a dot.

A float parsed with bit size 32 comes back as a `double` holding a value a `float` can represent exactly. Format it with bit size 32 too, or the shortest form is the one for a double, which has more digits than you want:

<!-- example: ../examples/strconv/strconv.c#float32 -->
```c
double g = strconv_parse_float(BURROW_S("0.1"), 32, NULL);
Str as32 = strconv_format_float(a, g, 'g', -1, 32);
Str as64 = strconv_format_float(a, g, 'g', -1, 64);
```

That prints `0.1` and `0.10000000149011612`. `strconv_append_float` adds the text to a byte slice, as the integer append forms do.

A number too big for the size is `strconv_err_range`, and you get an infinity of the right sign, as Go does. A number too small to be anything but zero is not an error:

<!-- example: ../examples/strconv/strconv.c#floaterr -->
```c
double inf = strconv_parse_float(BURROW_S("1e400"), 64, &err);
if (errors_is(err, strconv_err_range))
    printf("%s: " BURROW_STR_FMT "\n", inf > 0 ? "+Inf" : "?",
           BURROW_STR_ARG(error_text(err)));
```

The parser is a port of Go 1.27's, including the fast path that scales by a power of ten with a 128 bit multiply and the slow path that falls back to a big decimal when the fast one cannot decide, so it agrees with Go on every input, including the hard ones halfway between two doubles.

## Complex numbers

<!-- example: ../examples/strconv/strconv.c#complex -->
```c
Complex128 c = strconv_parse_complex(BURROW_S("(1.5-2i)"), 128, &err);
Str text = strconv_format_complex(a, c, 'g', -1, 128);
```

`strconv_parse_complex` takes a real part, an imaginary part ending in `i`, or both, optionally in parentheses, and returns a `Complex128`. A bit size of 64 rounds each part to a float. `strconv_format_complex` writes `(1.5-2i)`, each part formatted the way `strconv_format_float` would.

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
