# Formatted printing and scanning

`burrow/fmt.h` is Go's `fmt`, both halves. The verbs, flags and output of the printing functions match Go's byte for byte, and the test suite runs every case from Go's own `fmt_test.go` whose operand is a plain value. The scanning functions read text back into values the way Go's do, and their tests run every table in Go's `scan_test.go` and compare the count, the error and each value against what Go got.

What separates it from `printf` is that every operand carries its type. `%v` works on anything, a width counts runes and not bytes, a wrong verb prints a note in the output instead of undefined behaviour, and a type with a `String` method prints through it.

## Printing

Each function has a `_v` form that takes the operands directly:

<!-- example: ../examples/fmt/fmt.c#printf -->
```c
fmt_printf_v("%d items at %.2f each, %q\n", 3, 1.5, "tea");
```

That prints `3 items at 1.50 each, "tea"`. The `_v` macros wrap each operand in an `Any` with `BURROW_ANY_OF`, which picks the Go type from the C type: `int` and `Int` are `int`, `double` is `float64`, a string literal or `Str` is `string`, `bool` is `bool`, and so on. The table is in `burrow/iface.h`.

The one to watch is `true`. Before C23 it is an `int`, so `fmt_println_v(true)` prints `1`. Pass a `bool` variable or write `(bool)true`:

<!-- example: ../examples/fmt/fmt.c#println -->
```c
bool done = true;
fmt_println_v("sum:", 2 + 2, done, 0.5);
```

`fmt_println` puts a space between every pair of operands and a newline at the end. `fmt_print` adds a space only between two operands when neither is a string, as Go does.

`fmt_printf`, `fmt_print` and `fmt_println` write to standard output through C's `stdout`, so they mix safely with `printf`. `fmt_fprintf` and the other `f` forms write to any `IoWriter`. All of them return the number of bytes written and take an `Error *` for the write error, which the `_v` forms pass as `NULL`.

## Strings and byte slices

`fmt_sprintf` returns a `Str` allocated from the allocator you pass, at its exact length:

<!-- example: ../examples/fmt/fmt.c#sprintf -->
```c
Str s = fmt_sprintf_v(a, "[%6.2f|%-5s|%#x|%08b]", 3.14159, "ab", 255, 5);
```

That is `[  3.14|ab   |0xff|00000101]`. The output is built in a buffer on the stack first, so short results cost one allocation and long ones cost a few more while they are being built.

`fmt_appendf`, `fmt_append` and `fmt_appendln` add to a byte slice and return it, the same as `append` does in Go:

<!-- example: ../examples/fmt/fmt.c#slice -->
```c
Slice line = fmt_appendf_v(a, slice_nil(TYPE_BYTE), "id=%d", 42);
line = fmt_append_v(a, line, " ok");
```

The functions without `_v` take the operands as a slice of `Any`. That is the form to use when the operands are built at run time, when there are more than 32 of them, or when there are none at all, since a variadic macro needs at least one:

<!-- example: ../examples/fmt/fmt.c#explicit -->
```c
Any args[] = {BURROW_ANY_OF(7), BURROW_ANY_OF("seven")};
Str t =
    fmt_sprintf(a, BURROW_S("%[2]s is %[1]d"), slice_from(args, 2, 2, TYPE_ANY));
```

That also shows an explicit argument index. `%[2]s` takes the second operand, and the next verb carries on from wherever the last index left off.

## Verbs

The verbs are Go's, and so is what each one does to each type:

| Verb | Meaning |
| --- | --- |
| `%v` | the value in its default format, `%+v` adds struct field names, `%#v` is Go syntax |
| `%T` | the Go type of the value |
| `%%` | a percent sign |
| `%t` | `true` or `false` |
| `%d %b %o %O %x %X` | an integer in base 10, 2, 8, 8 with `0o`, 16 and 16 in capitals |
| `%c %q %U` | the character, a quoted character literal, and `U+1234` |
| `%e %E %f %F %g %G %x %X %b` | a float or complex number, as in `strconv_format_float` |
| `%s %q %x %X` | a string or byte slice, plainly, quoted, or in hex |
| `%p` | the address in a pointer, slice, map, channel or function |

The flags are `+`, `-`, `#`, space and `0`, then a width, then a precision after a dot. Either can be `*` to take it from an operand, which must be an integer.

## Values

A struct declared with `BURROW_STRUCT` prints the way Go prints a struct, because its descriptor has the field names and types:

<!-- example: ../examples/fmt/fmt.c#types -->
```c
#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT(Point, POINT_FIELDS);
```

A C struct has no type of its own for `BURROW_ANY_OF` to find, so a struct operand is passed as `BURROW_ANY` with its descriptor and its address:

<!-- example: ../examples/fmt/fmt.c#struct -->
```c
Point p = {1, 2};
Any v = BURROW_ANY(TYPE_OF(Point), &p);
fmt_printf_v("%v %+v %#v %T\n", v, v, v, v);
```

That prints `{1 2} {X:1 Y:2} Point{X:1, Y:2} Point`. Slices print as `[3 1 4]`, and maps print with their keys sorted so that the output is the same every time:

<!-- example: ../examples/fmt/fmt.c#map -->
```c
Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
BURROW_MAP_SET(Str, Int, m, BURROW_S("b"), 2);
BURROW_MAP_SET(Str, Int, m, BURROW_S("a"), 1);
Int xs[] = {3, 1, 4};
fmt_println_v(m, slice_from(xs, 3, 3, TYPE_INT));
```

That prints `map[a:1 b:2] [3 1 4]`. A pointer to a struct, slice, array or map prints as `&` and the value at the top level and as an address anywhere deeper, which is how Go avoids printing a cycle forever.

## Methods

A type whose descriptor has a `String` method taking nothing and returning `Str` is a Stringer, and `%v`, `%s`, `%q`, `%x` and `%X` print what it returns:

<!-- example: ../examples/fmt/fmt.c#stringer -->
```c
#define CELSIUS_FIELDS(F, T) F(T, double, Deg, "")
BURROW_STRUCT_DECL(Celsius, CELSIUS_FIELDS);

static Str celsius_string(Celsius *c) {
    return fmt_sprintf_v(error_allocator(), "%.1f°C", c->Deg);
}

#define CELSIUS_SIG_String(IN, OUT) OUT(Str)
#define CELSIUS_METHODS(M, T) M(T, String, celsius_string, CELSIUS_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(Celsius, CELSIUS_FIELDS, CELSIUS_METHODS);
```

fmt copies what the method returns and never frees it, the same way Go leaves the string to its collector. A method that builds its text should put it somewhere that gets cleaned up without the caller's help, and the goroutine's error arena is the handy place for that: `error_allocator()` is always there, and what goes in it dies with the goroutine or at the next `error_release`.

Any other verb prints the fields, so `%d` shows what is underneath:

<!-- example: ../examples/fmt/fmt.c#method -->
```c
Celsius c = {21.5};
fmt_printf_v("it is %v, or %d in the raw\n", BURROW_ANY(TYPE_OF(Celsius), &c),
             BURROW_ANY(TYPE_OF(Celsius), &c));
```

That prints `it is 21.5°C, or {%!d(float64=21.5)} in the raw`. The other methods work the same way. `Error` returning `Str` is used before `String`, `GoString` is used for `%#v`, and `Format` taking a `FmtState` and a `Rune` takes over the printing completely. A `Format` method writes with `st.vt->write`, asks for the flags with `st.vt->flag`, `st.vt->width` and `st.vt->precision`, and can get the directive back as text with `fmt_format_string`.

Methods are found through the descriptor, so a struct field prints through its method only when the field is exported, which is Go's rule. A method on a nil pointer is not called and `<nil>` is printed instead, since calling it would crash in C where Go would only panic. A method that panics does not take the program down: the panic is caught and printed in place of the value, as `%!v(PANIC=String method: the message)`.

## Mistakes in the format

A format that does not match its operands prints a note where the value would have gone rather than failing:

<!-- example: ../examples/fmt/fmt.c#bad -->
```c
Str s = fmt_sprintf_v(a, "%d %s|%d", "x", 5);
```

That is `%!d(string=x) %!s(int=5)|%!d(MISSING)`. The other notes are `%!(EXTRA type=value)` for operands left over, `%!d(BADINDEX)` for an index past the end, `%!(BADWIDTH)` and `%!(BADPREC)` for a `*` whose operand is not an integer, and `%!(NOVERB)` for a `%` at the end of the format.

## Errors

`fmt_errorf` formats a message and returns it as an `Error`. The verb `%w` prints an error operand the way `%v` would and also wraps it, so `errors_is` and `errors_as` can find it:

<!-- example: ../examples/fmt/fmt.c#errorf -->
```c
Error err = fmt_errorf_v("load %q: %w", "config.toml", err_not_found);
if (errors_is(err, err_not_found))
    fmt_println_v(err);
```

With one `%w`, `errors_unwrap` gives back the wrapped error. With more than one, the result wraps all of them, and `errors_is` looks through each. `%w` with an operand that is not an error, or anywhere outside `fmt_errorf`, prints `%!w(...)`. The error comes from `error_allocator()`, the calling goroutine's error arena, like the errors the rest of the library makes, so `error_retain` it into an allocator of your own when it has to outlive the goroutine.

## Scanning

There are three families, the same as in Go. `fmt_sscan` reads from a `Str`, `fmt_fscan` from an `IoReader` and `fmt_scan` from standard input. Each comes in three forms. The plain one treats newlines as spaces, the `ln` one stops at a newline and wants one after the last operand, and the `f` one follows a format. They return how many operands they filled and put the first error in `err`.

The `_v` forms take pointers to the operands, with the allocator and the error first:

<!-- example: ../examples/fmt/fmt.c#sscan -->
```c
Int n = 0;
Str item = {0};
double price = 0;
Error err = BURROW_NO_ERROR;
Int got = fmt_sscan_v(a, &err, "3 tea\n1.5", &n, &item, &price);
```

That fills all three and returns 3. The type of each operand comes from the C type of the pointer, the way `BURROW_ANY_OF` works when printing, so `int *` scans a 32 bit integer, `Int *` a Go `int`, `double *` a `float64`, `Str *` a string and `Slice *` a byte slice. Strings and byte slices are allocated from the allocator you pass, and so is anything a `%q` has to unquote.

A format matches its text literally, with the verbs Go has for each type:

<!-- example: ../examples/fmt/fmt.c#sscanf -->
```c
int h = 0, m = 0;
fmt_sscanf_v(a, &err, "at 09:45", "at %d:%d", &h, &m);
```

Spaces in the format match any run of spaces in the input, and a newline has to match a newline, which are Go's rules down to the corner cases. `%d`, `%x`, `%o` and `%b` read in that base, `%v` reads a Go literal with its base prefix and underscores, `%c` reads one rune without skipping space, and a width limits how many runes a verb reads.

When the input does not fit, the scan stops at that operand and `err` says why, with Go's text:

<!-- example: ../examples/fmt/fmt.c#scanerr -->
```c
Int x = 0;
if (fmt_sscan_v(a, &err, "ten", &x) != 1)
    fmt_println_v(err);
```

That prints `expected integer`. Running out of input between operands gives `io_eof`, and running out partway through something that had to finish, a quoted string or a literal in the format, gives `io_err_unexpected_eof`.

A type scans itself when its descriptor has a `Scan` method taking a `FmtScanState` and a `Rune` and returning `Error`. The state reads runes, skips space and hands out tokens, and `fmt_scan_state_reader` turns it into an `IoReader` so that the method can run a scan of its own over the same input:

<!-- example: ../examples/fmt/fmt.c#scanner -->
```c
#define RGB_FIELDS(F, T)                                                               \
    F(T, uint8_t, R, "")                                                               \
    F(T, uint8_t, G, "")                                                               \
    F(T, uint8_t, B, "")
BURROW_STRUCT_DECL(Rgb, RGB_FIELDS);

static Error rgb_scan(Rgb *c, FmtScanState st, Rune verb) {
    (void)verb;
    Error err = BURROW_NO_ERROR;
    fmt_fscanf_v(heap_allocator(), &err, fmt_scan_state_reader(&st), "#%2x%2x%2x",
                 &c->R, &c->G, &c->B);
    return err;
}

#define RGB_SIG_Scan(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
#define RGB_METHODS(M, T) M(T, Scan, rgb_scan, RGB_SIG_Scan)
BURROW_STRUCT_DEFINE_METHODS(Rgb, RGB_FIELDS, RGB_METHODS);
```

A struct is passed as `BURROW_ANY` with its descriptor and its address, as it is for printing:

<!-- example: ../examples/fmt/fmt.c#scan-method -->
```c
Rgb c = {0};
fmt_sscan_v(a, &err, "#ff8000", BURROW_ANY(TYPE_OF(Rgb), &c));
fmt_println_v(BURROW_ANY(TYPE_OF(Rgb), &c));
```

That prints `{255 128 0}`. A pointer of a type the `_v` macros do not know is a compile error rather than a scan that fails at run time.

`fmt_scan` and its siblings read standard input with `getc`, one byte at a time, so they share the stream with `scanf` and `fgets`. A scan from an `IoReader` also reads one byte per call, which is what Go does with a reader that has no `ReadRune`. As in Go, a scan may read one rune past the end of what it returns, to see that a number or a word has ended, and that rune is gone for the next call. It only matters when nothing separates one value from the next.

## Differences from Go

An `Any` holds a pointer to its value, and the `_v` forms put that value in a compound literal. The operands only live until the end of the statement, which is as long as the call needs them.

Method sets are those of the descriptor. Every method takes a pointer receiver in burrow, so a value and a pointer to it have the same methods, where Go gives a value only the methods with value receivers.

An error made by `errors_new` prints as `*errors.errorString` under `%T`, which is what Go says for one. An error with a `self_type` in its vtable reports that type instead.

Go's scan functions take `any` and check at run time that each operand is a pointer. The `_v` macros take pointers and check at compile time, and the plain functions take each operand as an `Any` whose data is the address to scan into, so there is no "type not a pointer" error.
