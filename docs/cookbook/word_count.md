# Count the words in a file

Read a file one word at a time with a `bufio` scanner split on `BUFIO_SCAN_WORDS`, count each word in a map, and print the five most used. The map has no order of its own, so the words come out of it into a slice that `slices_sort_func` puts in order, by count and then alphabetically.

<!-- example: ../examples/cookbook/word_count.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* More uses first, and alphabetical where two words are used as often. */
static int by_count(void *env, const void *x, const void *y) {
    Map *counts = env;
    Str a = *(const Str *)x, b = *(const Str *)y;
    Int na = *MAP_GET(Str, Int, counts, a), nb = *MAP_GET(Str, Int, counts, b);
    return cmp_or(cmp_compare(nb, na), cmp_compare(a, b));
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = os_write_file(
        S("dickens.txt"),
        BURROW_B("It was the best of times, it was the worst of times,\n"
                 "it was the age of wisdom, it was the age of foolishness,\n"
                 "it was the epoch of belief, it was the epoch of incredulity.\n"),
        0644);
    OsFile *f = os_open(a, S("dickens.txt"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    /* Split on spaces and newlines, then drop the punctuation and the case,
     * so "It" and "it," count as the same word. */
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
    BufioScanner *sc = bufio_new_scanner(a, os_file_as_io_reader(f));
    bufio_scanner_split(sc, BUFIO_SCAN_WORDS);
    Int total = 0;
    while (bufio_scanner_scan(sc)) {
        Str w =
            strings_to_lower(a, strings_trim(bufio_scanner_text(sc), S(".,;:!?\"'")));
        if (w.len == 0)
            continue;
        total++;
        Int *n = MAP_GET(Str, Int, counts, w);
        MAP_SET(Str, Int, counts, w, n != NULL ? *n + 1 : 1);
    }
    err = bufio_scanner_err(sc);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    Slice words = slices_collect(a, TYPE_STRING, maps_keys(counts));
    slices_sort_func(words, FN(SlicesCmpFunc, by_count, counts));
    Int nwords = map_len(counts);
    fmt_printf_v("%d words, %d different\n", total, nwords);
    for (Int i = 0; i < 5 && i < words.len; i++) {
        Str w = AT(Str, words, i);
        Int n = *MAP_GET(Str, Int, counts, w);
        fmt_printf_v("%-6s %d\n", w, n);
    }

    bufio_scanner_free(sc);
    os_file_close(f);
    arena_free(&ar);
}
```

It prints:

```text
36 words, 13 different
it     6
of     6
the    6
was    6
age    2
```

The program is [docs/examples/cookbook/word_count.c](../examples/cookbook/word_count.c). CI builds and runs it and checks that it prints this.
