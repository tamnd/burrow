# Print a map in a fixed order

Ranging over a map gives its keys in no fixed order, and the order can change from one run to the next. `slices_sorted` collects the keys from `maps_keys` and sorts them. `slices_sort_func` sorts by anything else, such as the values, with a tie-breaker to keep the order the same every run.

<!-- example: ../examples/cookbook/sorted_keys.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* Compares two keys of the map in env by their counts, highest first, and by
 * name when two counts are the same, so the order is the same every run. */
static int by_count(void *env, const void *x, const void *y) {
    Map *counts = env;
    Str a = *(const Str *)x, b = *(const Str *)y;
    Int na = *MAP_GET(Str, Int, counts, a);
    Int nb = *MAP_GET(Str, Int, counts, b);
    if (na != nb)
        return na > nb ? -1 : 1;
    return (int)strings_compare(a, b);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Map *langs = map_make(a, TYPE_STRING, TYPE_INT, 0);
    static const char *const votes[] = {"go",  "c",  "rust", "zig",
                                        "zig", "go", "zig",  "c"};
    for (size_t i = 0; i < sizeof votes / sizeof votes[0]; i++) {
        Str k = str_from_cstr(votes[i]);
        Int *n = MAP_GET(Str, Int, langs, k);
        MAP_SET(Str, Int, langs, k, n != NULL ? *n + 1 : 1);
    }

    /* Ranging over a map gives the keys in no fixed order, and it can
     * differ from one run to the next. For output a person reads, or a
     * test compares, collect the keys and sort them first. */
    Slice keys = slices_sorted(a, TYPE_STRING, maps_keys(langs));
    fmt_print_v("by name:");
    for (Int i = 0; i < keys.len; i++) {
        Str k = AT(Str, keys, i);
        fmt_printf_v(" %s=%d", k, *MAP_GET(Str, Int, langs, k));
    }
    fmt_print_v("\n");

    slices_sort_func(keys, FN(SlicesCmpFunc, by_count, langs));
    fmt_print_v("by votes:");
    for (Int i = 0; i < keys.len; i++) {
        Str k = AT(Str, keys, i);
        fmt_printf_v(" %s=%d", k, *MAP_GET(Str, Int, langs, k));
    }
    fmt_print_v("\n");
    arena_free(&ar);
}
```

It prints:

```text
by name: c=2 go=2 rust=1 zig=3
by votes: zig=3 c=2 go=2 rust=1
```

The program is [docs/examples/cookbook/sorted_keys.c](../examples/cookbook/sorted_keys.c). CI builds and runs it and checks that it prints this.
