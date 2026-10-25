# Take a file path apart

`filepath_join_v` builds a path from parts and cleans it, so doubled separators and `..` go away. `filepath_dir`, `filepath_base` and `filepath_ext` take it apart again, `filepath_rel` says how to get from one directory to a path, and `filepath_match` checks a name against a shell pattern. On Windows the separator is a backslash, so the output there is different.

<!-- example: ../examples/cookbook/file_paths.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Join puts in the separator and cleans up the result, so a doubled slash
     * or a .. in the parts does not end up in the path. The separator is the
     * system's own, so on Windows the output below has backslashes. */
    Str p = filepath_join_v(a, 4, S("data"), S("2026//reports"), S("../exports"),
                            S("sales.tar.gz"));
    fmt_println_v("path:", p);

    Str dir = filepath_dir(a, p);
    Str base = filepath_base(p);
    Str ext = filepath_ext(p);
    fmt_printf_v("dir %s, base %s, ext %s\n", dir, base, ext);

    /* Ext is only the last dot, so a name with two takes a second look. */
    Str stem = strings_trim_suffix(base, ext);
    Str ext2 = filepath_ext(stem);
    fmt_printf_v("stem %s, which still ends in %s\n", stem, ext2);

    Error err = NO_ERROR;
    Str rel = filepath_rel(a, S("data/2026/reports"), p, &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_println_v("from data/2026/reports:", rel);

    const char *names[] = {"sales.tar.gz", "sales.csv", "notes.txt"};
    for (int i = 0; i < 3; i++) {
        Str name = str_from_cstr(names[i]);
        bool csv = filepath_match(S("*.csv"), name, &err);
        bool sales = filepath_match(S("sales.*"), name, &err);
        fmt_printf_v("%-12s *.csv %-5t sales.* %t\n", name, csv, sales);
    }
    arena_free(&ar);
}
```

It prints:

```text
path: data/2026/exports/sales.tar.gz
dir data/2026/exports, base sales.tar.gz, ext .gz
stem sales.tar, which still ends in .tar
from data/2026/reports: ../exports/sales.tar.gz
sales.tar.gz *.csv false sales.* true
sales.csv    *.csv true  sales.* true
notes.txt    *.csv false sales.* false
```

The program is [docs/examples/cookbook/file_paths.c](../examples/cookbook/file_paths.c). CI builds and runs it and checks that it prints this.
