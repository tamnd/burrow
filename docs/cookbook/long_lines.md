# Read lines that might be very long

A `BufioScanner` stops with `bufio_err_too_long` on a line longer than 64 KiB, which a minified file or a big JSON value can easily be. `bufio_scanner_buffer` raises the limit. The scan loop ends the same way for the end of the input and for an error, so check `bufio_scanner_err` after it.

<!-- example: ../examples/cookbook/long_lines.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* Counts the lines in text, scanning with a limit on how long one can be. */
static void count_lines(Alloc *a, Str text, Int max) {
    StringsReader src;
    strings_reader_reset(&src, text);
    BufioScanner *sc = bufio_new_scanner(a, strings_reader_as_io_reader(&src));
    if (max > 0)
        bufio_scanner_buffer(sc, slice_make(a, TYPE_BYTE, 0, 1024), max);
    Int n = 0, longest = 0;
    while (bufio_scanner_scan(sc)) {
        n++;
        Int len = bufio_scanner_text(sc).len;
        if (len > longest)
            longest = len;
    }
    /* The loop ends the same way at the end of the input and on an error,
     * so the error has to be checked after it. */
    Error err = bufio_scanner_err(sc);
    if (FAILED(err))
        fmt_printf_v("stopped after %d lines: %v\n", n, err);
    else
        fmt_printf_v("%d lines, the longest %d bytes\n", n, longest);
    bufio_scanner_free(sc);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* One line of 100,000 bytes, such as a minified file or a big JSON
     * value, between two short ones. */
    Str text = fmt_sprintf_v(a, "first\n%s\nlast\n", strings_repeat(a, S("x"), 100000));

    /* A scanner gives up on a line longer than 64 KiB unless told it may
     * grow its buffer further. */
    count_lines(a, text, 0);
    count_lines(a, text, 1024 * 1024);
    arena_free(&ar);
}
```

It prints:

```text
stopped after 1 lines: bufio.Scanner: token too long
3 lines, the longest 100000 bytes
```

The program is [docs/examples/cookbook/long_lines.c](../examples/cookbook/long_lines.c). CI builds and runs it and checks that it prints this.
