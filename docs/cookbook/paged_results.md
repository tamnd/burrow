# Hide paging behind an iterator

An API that returns results a page at a time can be wrapped in an `IterSeq` that fetches the next page only when it needs one. The code that uses it is a plain `RANGE` loop over the results. Breaking out of the loop makes yield return false, so the sequence stops and no more pages are fetched.

<!-- example: ../examples/cookbook/paged_results.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* Stands in for an API that hands out results a page at a time. */
static Int fetch_page(Int page, Int *out, Int size) {
    fmt_printf_v("  (fetching page %d)\n", page);
    Int n = 0;
    for (Int i = 0; i < size; i++) {
        Int id = page * size + i + 1;
        if (id > 23) /* there are 23 results in all */
            break;
        out[n++] = id;
    }
    return n;
}

/* A sequence of every result, one at a time, that fetches the next page only
 * when it runs out. When yield returns false the caller has seen enough, and
 * no more pages are fetched. Each value is a pointer to an Int. */
static void all_results(void *env, IterYield yield) {
    (void)env;
    Int buf[10];
    for (Int page = 0;; page++) {
        Int n = fetch_page(page, buf, 10);
        for (Int i = 0; i < n; i++) {
            if (!CALLF(yield, &buf[i]))
                return;
        }
        if (n < 10)
            return; /* a short page is the last one */
    }
}

static void run(void *env) {
    (void)env;
    IterSeq results = FN(IterSeq, all_results, NULL);

    /* The loop reads like a range over a slice, and doesn't know about
     * pages at all. */
    Int count = 0;
    RANGE(Int, id, results) {
        (void)id;
        count++;
    }
    fmt_println_v("all of them:", count);

    /* Stopping early stops the fetching too. */
    fmt_println_v("the first one over 12:");
    RANGE(Int, id, results) {
        if (id > 12) {
            fmt_println_v(" ", id);
            break;
        }
    }
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
```

It prints:

```text
  (fetching page 0)
  (fetching page 1)
  (fetching page 2)
all of them: 23
the first one over 12:
  (fetching page 0)
  (fetching page 1)
  13
```

The program is [docs/examples/cookbook/paged_results.c](../examples/cookbook/paged_results.c). CI builds and runs it and checks that it prints this.
