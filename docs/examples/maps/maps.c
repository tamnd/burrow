#include <math.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

static void tour(Alloc *a) {
    // doc: tour
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);

    BURROW_MAP_SET(Str, Int, counts, BURROW_S("the"), 1);

    Int *n = BURROW_MAP_GET(Str, Int, counts, BURROW_S("the"));
    if (n != NULL)
        (*n)++;
    // doc: end
    printf("the %lld\n", (long long)*BURROW_MAP_GET(Str, Int, counts, BURROW_S("the")));
}

static void raw(Alloc *a) {
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);

    // doc: raw
    Str word = BURROW_S("the");
    Int one = 1;
    map_set(counts, &word, &one);
    // doc: end
    printf("len %lld\n", (long long)map_len(counts));
}

static void macros(Alloc *a) {
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);

    // doc: macros
    BURROW_MAP_SET(Str, Int, counts, BURROW_S("the"), 1);
    Int *n = BURROW_MAP_GET(Str, Int, counts, BURROW_S("the"));
    bool have = BURROW_MAP_HAS(Str, counts, BURROW_S("the"));
    BURROW_MAP_DEL(Str, counts, BURROW_S("the"));
    // doc: end
    (void)n; /* dangling now, the entry it pointed at is gone */
    printf("have %d, len after delete %lld\n", have, (long long)map_len(counts));
}

static void get(Alloc *a) {
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Str key = BURROW_S("missing");
    Int out = -1;

    // doc: get
    Int *v = map_get(m, &key);         /* m[key], or NULL when it is not there */
    bool ok = map_get2(m, &key, &out); /* v, ok := m[key] */
    // doc: end
    printf("v %s, ok %d, out %lld\n", v == NULL ? "NULL" : "set", ok, (long long)out);
}

static Error store(Alloc *a, Map *m, Str key, Int val) {
    // doc: set
    if (!map_set(m, &key, &val))
        return errors_new(a, BURROW_S("out of memory"));
    // doc: end
    return (Error){0};
}

static void seen(Alloc *a) {
    Str word = BURROW_S("gopher");

    // doc: seen
    Map *seen = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    map_set(seen, &word, NULL);
    if (map_get(seen, &word) != NULL) {
        printf("seen " BURROW_STR_FMT " before\n", BURROW_STR_ARG(word));
    }
    // doc: end
}

static void del(Alloc *a) {
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Str key = BURROW_S("a");
    BURROW_MAP_SET(Str, Int, m, BURROW_S("a"), 1);
    BURROW_MAP_SET(Str, Int, m, BURROW_S("b"), 2);

    // doc: del
    map_del(m, &key); /* delete(m, key) */
    map_clear(m);     /* clear(m) */
    // doc: end
    printf("len after clear %lld\n", (long long)map_len(m));
}

static void iter(Alloc *a) {
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    BURROW_MAP_SET(Str, Int, m, BURROW_S("only"), 1);

    // doc: iter
    const void *k;
    void *v;
    for (MapIter it = map_iter(m); map_next(&it, &k, &v);) {
        printf(BURROW_STR_FMT " = %lld\n", BURROW_STR_ARG(*(const Str *)k),
               (long long)*(Int *)v);
    }
    // doc: end
}

static void zero(Alloc *a) {
    Map *m = map_make(a, TYPE_FLOAT64, TYPE_INT, 0);

    // doc: zero
    BURROW_MAP_SET(double, Int, m, -0.0, 7);
    BURROW_MAP_HAS(double, m, 0.0); /* true, and len is 1 */
    // doc: end
    printf("has 0.0 %d, len %lld\n", BURROW_MAP_HAS(double, m, 0.0),
           (long long)map_len(m));
}

static void nans(Alloc *a) {
    Map *m = map_make(a, TYPE_FLOAT64, TYPE_INT, 0);
    double nan = NAN;
    Int one = 1;

    // doc: nan
    map_set(m, &nan, &one);
    map_set(m, &nan, &one);
    map_len(m);       /* 2 */
    map_get(m, &nan); /* NULL */
    // doc: end
    printf("len %lld, found %d\n", (long long)map_len(m), map_get(m, &nan) != NULL);
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    tour(a);
    raw(a);
    macros(a);
    get(a);

    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Error err = store(a, m, BURROW_S("x"), 3);
    printf("store ok %d\n", BURROW_OK(err));

    seen(a);
    del(a);
    iter(a);
    zero(a);
    nans(a);
    arena_free(&ar);
    return 0;
}

/* Output:
the 2
len 1
have 1, len after delete 0
v NULL, ok 0, out 0
store ok 1
seen gopher before
len after clear 0
only = 1
has 0.0 1, len 1
len 2, found 0
*/
