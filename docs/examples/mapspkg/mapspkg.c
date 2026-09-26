#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

/* Prints a map[string]int in key order, since the map's own order is random. */
static void print_map(Alloc *a, Map *m) {
    Slice keys = slices_sorted(a, TYPE_STRING, maps_keys(m));
    for (Int i = 0; i < keys.len; i++) {
        Str k = BURROW_AT(Str, keys, i);
        printf(i == 0 ? "%.*s:%lld" : " %.*s:%lld", (int)k.len, k.p,
               (long long)*BURROW_MAP_GET(Str, Int, m, k));
    }
    printf("\n");
    mem_free(a, keys.p, (size_t)keys.cap * sizeof(Str), _Alignof(Str));
}

// doc: pred
/* Go's func(k string, v int) bool, true for the odd values. */
static bool is_odd(void *env, const void *k, const void *v) {
    return *(const Int *)v % 2 != 0;
}
// doc: end

int main(void) {
    Alloc *a = heap_allocator();

    // doc: clone
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    BURROW_MAP_SET(Str, Int, m, BURROW_S("one"), 1);
    BURROW_MAP_SET(Str, Int, m, BURROW_S("two"), 2);
    BURROW_MAP_SET(Str, Int, m, BURROW_S("three"), 3);

    Map *c = maps_clone(a, m);
    printf("%d\n", maps_equal(m, c)); /* 1 */
    BURROW_MAP_SET(Str, Int, c, BURROW_S("four"), 4);
    printf("%d\n", maps_equal(m, c)); /* 0 */
    // doc: end

    // doc: delete
    maps_delete_func(c, BURROW_FN(MapsPredFunc, is_odd, NULL));
    print_map(a, c); /* four:4 two:2 */
    // doc: end

    // doc: copy
    maps_copy(c, m);
    print_map(a, c); /* four:4 one:1 three:3 two:2 */
    // doc: end

    // doc: iter
    Map *d = maps_collect(a, TYPE_STRING, TYPE_INT, maps_all(m));
    print_map(a, d); /* one:1 three:3 two:2 */
    // doc: end

    map_free(d);
    map_free(c);
    map_free(m);
    return 0;
}

/* Output:
1
0
four:4 two:2
four:4 one:1 three:3 two:2
one:1 three:3 two:2
*/
