# Count the status codes in a log

Read a log a line at a time with a `BufioScanner` and pull fields out of each line with a regular expression. A line that doesn't match is reported with its line number and skipped. `bufio_scanner_err` after the loop says whether the loop stopped at the end of the file or because something went wrong.

<!-- example: ../examples/cookbook/log_status.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = os_write_file(S("access.log"),
                              BURROW_B("10.0.0.1 GET /index.html 200\n"
                                       "10.0.0.2 GET /missing 404\n"
                                       "not a request\n"
                                       "10.0.0.1 POST /login 302\n"
                                       "10.0.0.3 GET /index.html 200\n"),
                              0644);
    OsFile *f = os_open(a, S("access.log"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    /* The method, the path and the status, one line at a time. */
    Regexp *re = regexp_must_compile(a, S("^\\S+ (\\S+) (\\S+) (\\d{3})$"));
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
    BufioScanner *sc = bufio_new_scanner(a, os_file_as_io_reader(f));
    Int lineno = 0;
    while (bufio_scanner_scan(sc)) {
        lineno++;
        Str line = bufio_scanner_text(sc);
        Slice m = regexp_find_string_submatch(re, a, line);
        if (m.len == 0) {
            fmt_printf_v("line %d doesn't parse: %q\n", lineno, line);
            continue;
        }
        Str status = AT(Str, m, 3);
        Int *n = MAP_GET(Str, Int, counts, status);
        MAP_SET(Str, Int, counts, status, n != NULL ? *n + 1 : 1);
    }
    err = bufio_scanner_err(sc);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_println_v(counts);

    bufio_scanner_free(sc);
    regexp_free(re);
    os_file_close(f);
    arena_free(&ar);
}
```

It prints:

```text
line 3 doesn't parse: "not a request"
map[200:2 302:1 404:1]
```

The program is [docs/examples/cookbook/log_status.c](../examples/cookbook/log_status.c). CI builds and runs it and checks that it prints this.
