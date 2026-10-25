# Sort records by more than one key

Sort a list of results by score, highest first. `slices_sort_stable_func` keeps results with the same score in the order they came in. When that order means nothing, `cmp_or` adds a second key, here the name, and `slices_sort_func` is enough.

<!-- example: ../examples/cookbook/sort_records.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

#define RESULT_FIELDS(F, T)                                                            \
    F(T, Str, Name, "")                                                                \
    F(T, Int, Score, "")
BURROW_STRUCT(Result, RESULT_FIELDS);

/* Highest score first. */
static int by_score(void *env, const void *x, const void *y) {
    (void)env;
    const Result *a = x, *b = y;
    return cmp_compare(b->Score, a->Score);
}

/* Highest score first, and by name where two scores are the same. */
static int by_score_then_name(void *env, const void *x, const void *y) {
    const Result *a = x, *b = y;
    return cmp_or(by_score(env, x, y), cmp_compare(a->Name, b->Name));
}

static void show(Slice rs) {
    for (Int i = 0; i < rs.len; i++) {
        Result r = AT(Result, rs, i);
        fmt_printf_v("%d. %-8s %d\n", i + 1, r.Name, r.Score);
    }
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Result rows[] = {
        {S("Ada"), 90}, {S("Grace"), 85},   {S("Linus"), 70},
        {S("Ken"), 85}, {S("Barbara"), 90}, {S("Dennis"), 70},
    };
    Int n = sizeof rows / sizeof rows[0];
    Slice rs = slice_from(rows, n, n, TYPE_OF(Result));

    /* A stable sort keeps ties in the order they came in. */
    Slice stable = slices_clone(a, rs);
    slices_sort_stable_func(stable, FN(SlicesCmpFunc, by_score, NULL));
    show(stable);

    /* Or break the ties with a second key. */
    fmt_println_v("--");
    slices_sort_func(rs, FN(SlicesCmpFunc, by_score_then_name, NULL));
    show(rs);
    arena_free(&ar);
}
```

It prints:

```text
1. Ada      90
2. Barbara  90
3. Grace    85
4. Ken      85
5. Linus    70
6. Dennis   70
--
1. Ada      90
2. Barbara  90
3. Grace    85
4. Ken      85
5. Dennis   70
6. Linus    70
```

The program is [docs/examples/cookbook/sort_records.c](../examples/cookbook/sort_records.c). CI builds and runs it and checks that it prints this.
